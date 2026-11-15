# Existing outer signal-chain regression

The ARM64 Android fixture starts ART and HotSpot in one application process,
then installs a forwarding SIGSEGV handler through bionic's explicit libc
handle before loading the bridge. That handler preserves the existing kernel
chain. Late installation must retain it without asking ART to reorder the
chain. The pre-existing public SIGSEGV action is compared before and after
installation (handler, flags and mask).

The run passed three System.load / unload cycles, 25 contract checks and three
8-thread pressure rounds (8,000 calls and 20 Guest overlaps each). Suspend/resume
was delivered 90 times, including three deliveries with Guest TLS active. VM
destruction completed; one idle domain remained pinned until process disposal,
as required by the experiment's lifetime contract. This is compatibility
coverage, not a performance measurement or a general production-support claim.

`native-report.txt` is the complete fixture output. `native-manifest.json`
records the native artifacts and source inputs. `summary.json` includes the
APK digest and the explicit outer-chain assertion, checked against the retained
report. The earlier standalone/ART results remain in `../late-signals`.

The regression was motivated by a Pojav startup failure: invoking
`EnsureFrontOfChain` during installation created repeated ART
`SignalChain::Handler` frames. Installation now validates ownership without
mutating the pre-existing handler ordering. Launcher-specific FPS evidence is
kept in the separate Pojav validation project.
