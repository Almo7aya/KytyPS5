● Key detail: `g_submission_pause_depth` — the pause is ALREADY ref-counted per... wait: `if
   (g_submission_pause_depth++ == 0) g_gpu->PauseSubmissions();` — if depth>0, the lock is a no-op (nested on
   the SAME thread — g_submission_pause_depth is thread_local presumably). Let me check:
   `g_submission_pause_depth` — thread_local? Used in GraphicsRunSubmissionLockHeld. It's likely `static
   thread_local uint32_t`. So nesting on the same thread is cheap, but ACROSS threads each takes
   m_submission_mutex — the herd.

   The coalescing design needs cross-thread sharing: when thread A holds the pause (depth 1, worker parked),
   thread B's GraphicsRunSubmissionLock should JOIN A's pause instead of blocking on m_submission_mutex.
   Requirements per participant: the worker stays paused (and the drain fence+label flush happened) until
   THEY finish; resume only when ALL participants leave.

   Implementation sketch:

   ```cpp
   // in Gpu class or file scope:
   static std::mutex g_pause_share_mutex;
   static uint32_t   g_pause_participants = 0;  // guarded by g_pause_share_mutex
   static bool       g_pause_drained      = false;

   GraphicsRunSubmissionLock::GraphicsRunSubmissionLock() {
       ... checks ...
       if (g_submission_pause_depth++ == 0) {
           g_gpu->PauseSubmissionsShared();
       }
   }
   ~ {
       if (--g_submission_pause_depth == 0) {
           g_gpu->ResumeSubmissionsShared();
       }
   }
   ```

   Gpu::PauseSubmissionsShared():
   ```cpp
   std::lock_guard lk(g_pause_share_mutex);
   if (g_pause_participants++ == 0) {
       // first participant: become the drainer
       m_submission_mutex.Lock();          // serialize vs Done()
       RequestPauseAndWait();              // park worker
       m_gfx_cp->BufferWait();             // fence submitted work
       LabelDrain();                       // flush labels
   }
   // joiners (participants>1) skip the drain: the first's drain already fenced everything
   // submitted so far...
   ```

   WAIT — correctness issue: a joiner that arrives AFTER the first's BufferWait: its fault may involve GPU
   work submitted after the first's fence?? The fence must cover the GPU work relevant to THE JOINER's fault.
   But faults are for pages the GPU touched BEFORE the fault (the faulting access already
   happened/attempted). BufferWait fences "all submitted work" at the time it runs. If the first
   participant's BufferWait ran at T1, and joiner J faults at T2>T1 — the GPU work that touched J's page was
   submitted BEFORE J's fault (before T2) — but was it submitted before T1? NOT NECESSARILY! The worker could
   have submitted new work between T1 and T2... NO — the worker is PARKED since T1 (RequestPauseAndWait keeps
   it parked until ResumeSubmissions). So no new GPU work is submitted while the pause is active. Everything
   the GPU had in flight at T1 is fenced by the first's BufferWait; between T1 and T2 nothing new is
   submitted (parked). So a single BufferWait at T1 covers all joiners' pages ✓✓. Labels likewise drained
   once ✓.

   BUT: what about GPU work submitted by threads DIRECTLY (not via the worker)? If any, BufferWait wouldn't
   cover them anyway — same as today's semantics. ✓

   So the shared drain is correct: park once, fence once, flush once, resolve concurrently, resume when the
   last leaves.

   One more subtlety: the drain (BufferWait+LabelDrain) must COMPLETE before any joiner resolves its fault. A
   joiner arriving while the first is still draining must WAIT for drain completion. So:

   ```cpp
   PauseSubmissionsShared():
       lock(g_pause_share_mutex);
       if (g_pause_participants == 0) {
           // leader
           m_pause_share_draining = true;
           unlock;
           m_submission_mutex.Lock();       // serialize vs Done
           RequestPauseAndWait();
           m_gfx_cp->BufferWait();
           LabelDrain();
           lock;
           m_pause_share_draining = false;
           g_pause_participants = 1;  (leader counts as a participant)
           m_pause_share_cv.SignalAll();    // drain done
           unlock;
           m_submission_mutex... — wait, m_submission_mutex: when is it released?
       } else {
           while (m_pause_share_draining) m_pause_share_cv.Wait(&g_pause_share_mutex);
           g_pause_participants++;
       }
   ```

   The m_submission_mutex (vs Done()): Done() takes it for the full-queue drain. Who holds it during a shared
   window? If the leader holds it from drain until the last participant leaves, Done() blocks for the whole
   window (fine — matches today's serialization vs Done). But then ResumeSubmissionsShared (last leaver)
   releases it:
   - Leader: takes m_submission_mutex at drain time; leaver (possibly a different thread) releases it when
   participants hit 0. Cross-thread mutex handoff — m_submission_mutex is Common::Mutex (probably a recursive
   or plain mutex with owner tracking?) — cross-thread unlock is UB for std::mutex! Common::Mutex might allow
   it or not. Risky.

   Alternative: keep m_submission_mutex HELD by the leader for the whole window, and the leader ALSO does the
   resume (last leaver signals the leader to release+resume). But the leader's PauseSubmissionsShared
   returned long ago (it's resolving its own fault). Restructure: the resume doesn't need m_submission_mutex
   — it needs ReleasePause (unpark worker) + m_submission_mutex.Unlock() to let Done() in. What if: last
   leaver does ReleasePause() (unpark, no mutex needed) and then m_submission_mutex.Unlock() — the unlock
   cross-thread is the problem.

   OR: have the leader hold m_submission_mutex ONLY during the drain (T1..drain done), then release it.
   Done() can then enter DURING the resolution window — is that safe? Done() does a full WaitLocked drain: it
   waits for the worker to drain the queue — but the worker is PARKED (pause still active via
   participants>0)... Done()'s WaitForIdle would block until the worker un-parks (window closes) ✓ — same as
   today's Done-vs-fault serialization, just achieved via the park instead of the mutex. Hmm — but does
   Done() ALSO need mutual exclusion against the fault handlers' cache mutations? That's what
   m_resource_mutex (FaultScope) is for (each fault holds it individually). Done() presumably doesn't touch
   the caches directly. The original code held m_submission_mutex across the whole fault — the doc §4.1 says
   "Mutual exclusion between faults and Done() is the original m_submission_mutex" — holding it for the whole
   fault was the conservative choice.

   Risk assessment for releasing m_submission_mutex after the drain: while the window is open, Done()
   acquires it and parks... Done calls WaitLocked (its own full drain) — but worker parked → Done waits for
   park release → Done holds m_submission_mutex while waiting → if a NEW fault arrives, it joins the window
   (no mutex needed) ✓ no deadlock. When the window closes (last leaver → ReleasePause), worker drains, Done
   completes, releases mutex ✓. The danger would be Done() racing a fault's cache resolution — but faults
   resolve under m_resource_mutex; Done's drain doesn't touch caches (it fences GPU). Should be safe.

   Hmm — actually, is even the LEADER's m_submission_mutex needed at all? The drain = park worker +
   BufferWait + LabelDrain. BufferWait fences submitted work; Done() also fences. Do they conflict? Two
   BufferWaits concurrently (Done + leader) on the same scheduler — probably not reentrant-safe. Keep
   m_submission_mutex around JUST the drain (leader) to serialize the drain against Done()'s drain. Short
   hold → no cross-thread issue ✓.

   So final design:

   ```cpp
   Gpu::PauseSubmissionsShared() {
       std::unique_lock lk(g_share.m_mutex);
       if (g_share.m_participants == 0) {
           g_share.m_draining = true;
           lk.unlock();
           {
               m_submission_mutex.Lock();
               RequestPauseAndWait();
               m_gfx_cp->BufferWait();
               LabelDrain();
               m_submission_mutex.Unlock();
           }
           lk.lock();
           g_share.m_draining = false;
           g_share.m_participants = 1;
           g_share.m_cv.SignalAll();
       } else {
           while (g_share.m_draining) g_share.m_cv.Wait(&g_share.m_mutex);
           g_share.m_participants++;
       }
   }

   Gpu::ResumeSubmissionsShared() {
       std::lock_guard lk(g_share.m_mutex);
       if (--g_share.m_participants == 0) {
           ReleasePause();   // unpark worker (takes m_queue_mutex internally)
       }
   }
   ```

   Done() keeps using m_submission_mutex + WaitLocked (its full drain) — unchanged. The g_gpu_mutex_owned
   thread_local flag (used by PauseSubmissions/ResumeSubmissions sanity checks and GraphicsRunGpuLockHeld) —
   set it in the shared variants too? It's per-thread; the shared window has multiple threads — the flag's
   meaning ("this thread owns the gpu mutex") breaks. GraphicsRunGpuLockHeld is used in
   bufferCache/textureCache fault-lock checks (lines 262/399/938). Setting it for each participant while they
   hold the window is... semantically they're all "inside" the pause. Hmm — those checks EXIT if a cache lock
   is taken while holding the gpu lock (lock-order inversion guards). Setting g_gpu_mutex_owned for
   participants preserves those invariants ✓. But the flag also gates PauseSubmissions's "already paused by
   this thread" EXIT — shared path doesn't call that. Let me keep g_gpu_mutex_owned managed per participant
   (set true on join, false on leave) so the guard checks behave as before. Hmm — wait, but
   GraphicsRunSubmissionLockHeld (g_submission_pause_depth) is already per-thread ✓.

   Also the WaitWatch/DrainStats instrumentation: keep counts (pause_count per participant? or per window?).
   Keep simple: count joins as pause_count for stats compatibility.

   DEADLOCK AUDIT vs the run-7 quiescent deadlock:
   - The worker (CP thread) faults: takes the CP path (no submission lock at all) — unchanged ✓.
   - Label callback: LabelInCallback path (resource mutex only) — unchanged ✓.
   - Other threads (guest/driver): shared window. A driver thread blocked in Done() while a window is open:
   Done holds m_submission_mutex, waits for park release; participants resolve; last leaver ReleasePause;
   Done proceeds ✓.
   - Can a participant block forever? A participant resolving its fault may wait on the readback (BufferCache
   Download path → GraphicsRunFinishScheduler is CP-only; non-CP Download → m_readback->Request + Complete
   waits for the readback WORKER thread). The readback worker does GPU transfers — needs... the readback
   thread doesn't need submissions (it's the Transfer path — independent queue?). If the readback worker
   needs the GPU worker unparked... Transfer::DownloadTiledImage uses the Vulkan queue directly (not the
   guest submission worker) — should proceed during the pause ✓ (that's how it works today — the per-fault
   drain also holds the pause during readback).
   - The §4.5 cycle: worker waits for label; label callback free (resource mutex only) ✓. The
   driver-thread-on-mutex case (the tid=0 gpu_pause_lock from run 7): with sharing, the driver thread JOINS
   the window (if one is open) instead of blocking on m_submission_mutex — the mutex is only held during the
   SHORT drain ✓ this actually FIXES the run-7 cycle (Done()'s m_submission_mutex no longer blocks the driver
   thread's fault for the whole Done wait)!! Wait — no: if Done() holds m_submission_mutex and a window is
   NOT open, the driver thread's shared-pause leader must take m_submission_mutex for the drain → blocks on
   Done (same as today) → Done waits for the worker... the run-7 cycle had worker stuck → Done stuck → mutex
   held → driver thread stuck → (label writes stuck?) → worker stuck. With sharing, when the driver thread is
   a JOINER (window open), no mutex wait ✓; when it's the LEADER (no window), it waits on Done's mutex (same
   as today). The run-7 cycle: was a window open? The worker was in cp_fault (which doesn't use the
   submission lock) — no window → driver thread = leader → waits on Done's mutex → cycle persists. Hmm.

   BUT — wait, reconsider the run-7 cycle: does the DRIVER thread's fault actually need the pause? It's the
   Vulkan driver writing query/label data for work the GPU already retired (like §4.5's label callback). The
   §4.5 fix bypassed the pause for LABEL CALLBACKS only. A VULKAN DRIVER thread faulting on tracked memory —
   its writes are also for retired work?? Not necessarily (driver writes timestamps at various points).
   Bypassing the pause for driver threads is riskier. Hmm.

   Actually — let me reconsider the run-7 deadlock cycle more carefully with the drain-during-Done knowledge:
   - AgcSubmissionThread: Done() → holds m_submission_mutex → WaitForIdle → waits for worker to drain queue.
   - Worker: cp_fault → stuck somewhere (unknown — pm_spin? rbreq? we never got the localized scope data
   because run 7 predates the scopes, and runs 8/9/10 didn't reproduce the quiescent stall).
   - Driver thread (tid=0): gpu_pause_lock → blocked on m_submission_mutex (held by Done).
   - If the worker's wait depends on the driver thread (label write), cycle.

   With sharing, if the driver thread had been a JOINER it wouldn't block — but no window was open. The fix
   for THAT is at the worker's stuck point (unknown yet). The sharing fix is still worthwhile for the herd
   (perf).

   OK — let me implement the shared pause. I'll write it carefully in graphicsRun.cpp. First look at the Gpu
   class members to place the shared state, and Common::Mutex/CV primitives (Common::CondVar usage:
   m_work_available.Signal(), m_idle.Wait(&mutex)).

   Let me view the Gpu class definition (around line 90-150).

 ● The per-thread pause depth is already ref-counted; the herd is cross-thread. I'll implement a shared pause
   window: first participant drains once (park + BufferWait + LabelDrain under m_submission_mutex), joiners
   attach without the mutex, last leaver un-parks. Single drain covers all joiners because the worker stays
   parked the whole window — nothing new gets submitted. Let me see the Gpu class and sync primitives:
