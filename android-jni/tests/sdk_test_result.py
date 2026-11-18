"""Preserve consumer observations on failure, timeout, and success."""

import json
import signal
import subprocess


def valid_registration_observations(rows):
    cases = [r for r in rows if r.get("stage") == "guest-registration"]
    return [r.get("alias") for r in cases] == [
        "signal",
        "bsd_signal",
        "ssignal",
        "sysv_signal",
        "__sysv_signal",
        "__sigaction",
        "siginterrupt",
        "sigignore",
        "sigset",
    ] and all(
        r.get("value") == 73
        and r.get("faults") == (0 if r["alias"] == "sigignore" else 1)
        for r in cases
    )


def valid_wait_observations(rows, summary):
    waits = [r for r in rows if r.get("stage") == "guest-wait"]
    unwinds = [r for r in rows if r.get("stage") == "wait-unwind"]
    if [r.get("mode") for r in waits] != list(range(60, 79)):
        return False
    skipped = 0
    for row in waits:
        if row.get("value") == 74 and row["mode"] in (67, 72):
            if row.get("skip") != "ENOSYS":
                return False
            skipped += 1
        elif row.get("value") != 73 or row.get("skip") is not None:
            return False
        elif row["mode"] in range(68, 73) and not row.get("interrupts", 0):
            return False
    return (
        skipped in (0, 2)
        and summary.get("guest_wait_skipped") == skipped
        and [r.get("saved_mask") for r in unwinds] == [0, 1]
        and all(
            r.get("kernel_wait") is True and r.get("subsequent_call") is True
            for r in unwinds
        )
    )


def valid_guest_target_observations(rows):
    targets = [r for r in rows if r.get("stage") == "guest-target"]
    return targets == [
        {
            "stage": "guest-target",
            "round": round,
            "depth": depth,
            "captures": captures,
            "saved_mask": saved,
            "value": 73,
            "checked": checked,
            "subsequent_call": True,
            "host_mask_preserved": True,
            "java_frames_crossed": False,
        }
        for round in range(2)
        for depth, captures, saved, checked in (
            (1, 1, 1, False),
            (1, 1, 0, False),
            (3, 1, 1, False),
            (5, 300, 0, False),
            (3, 1, 1, True),
        )
    ]


def valid_jni_resource_recovery(rows):
    host = [r for r in rows if r.get("stage") == "jni-resource-host"]
    guest = [r for r in rows if r.get("stage") == "jni-resource-guest"]
    boundary = [r for r in rows if r.get("stage") == "jni-managed-boundary"]
    return (
        host
        == [
            {
                "stage": "jni-resource-host",
                "api": name,
                "discarded": 1,
                "pending_preserved": name == "GetStringUTFChars",
            }
            for name in (
                "NewLocalRef",
                "NewGlobalRef",
                "NewWeakGlobalRef",
                "GetStringChars",
                "GetStringUTFChars",
                *(
                    "Get" + kind + "ArrayElements"
                    for kind in (
                        "Boolean",
                        "Byte",
                        "Char",
                        "Short",
                        "Int",
                        "Long",
                        "Float",
                        "Double",
                    )
                ),
                "GetStringCritical",
                "GetPrimitiveArrayCritical",
                "PushLocalFrame",
                "MonitorEnter",
                "GetStaticMethodID",
                "GetStaticFieldID",
            )
        ]
        and guest
        == [
            {
                "stage": "jni-resource-guest",
                "transfer": transfer,
                "discarded": 3,
                "lifo": True,
                "stale_token_rejected": True,
                "copyout_unchanged": True,
                "reentered_jni": True,
            }
            for transfer in ("siglongjmp", "setcontext")
        ]
        and boundary
        == [
            {
                "stage": "jni-managed-boundary",
                "rejected": True,
                "monitor_released": True,
                "finally_ran": True,
            }
        ]
    )


def valid_jni_signal_recovery(rows):
    return [r for r in rows if r.get("stage") == "jni-resource-signal"] == [
        {
            "stage": "jni-resource-signal",
            "api": name,
            "transfer": transfer,
            "discarded": 1,
            "same_carrier": True,
        }
        for name, transfer in (
            ("GetPrimitiveArrayCritical", "siglongjmp"),
            ("GetStringCritical", "setcontext"),
            ("NewGlobalRef", "setcontext"),
        )
    ]


def valid_signal_unwind_observations(rows):
    return [r for r in rows if r.get("stage") == "guest-signal-unwind"] == [
        {
            "stage": "guest-signal-unwind",
            "mode": mode,
            "value": 73,
            "handler_cleanup": True,
            "body_cleanup": True,
            "subsequent_call": True,
            "java_frames_crossed": False,
        }
        for mode in range(97, 101)
    ]


def valid_relay_lifecycle(rows, mode):
    return [r for r in rows if r.get("stage") == "runtime-relay-lifecycle"] == [
        {
            "stage": "runtime-relay-lifecycle",
            "preclaim_failures": 2,
            "retry": True,
            "borrowed": "borrowed-relay" in mode,
            "shutdown_preserved_owner": True,
            "masks_preserved": True,
            "stopped_owner_masked": True,
            "stopped_owner_parked": True,
        }
    ]


def execute_case(
    command,
    out,
    mode,
    timeout,
    env=None,
    require_pending=False,
    fatal=None,
    require_host_fault_mask=False,
    require_guest_target=False,
    fpe_optional=False,
    require_host_private=False,
):
    timed_out = False
    try:
        result = subprocess.run(
            command, capture_output=True, text=True, timeout=timeout, env=env
        )
        output = result.stdout + result.stderr
        returncode = result.returncode
    except subprocess.TimeoutExpired as error:
        timed_out = True
        parts = [error.stdout or b"", error.stderr or b""]
        output = "".join(
            p.decode(errors="replace") if isinstance(p, bytes) else p
            for p in parts
        )
        returncode = None

    (out / (mode + ".log")).write_text(output)

    rows = []
    malformed = False
    for line in output.splitlines():
        if line.startswith("{"):
            try:
                row = json.loads(line)
                if isinstance(row, dict):
                    rows.append(row)
                else:
                    malformed = True
            except json.JSONDecodeError:
                malformed = True

    terminal = [
        r for r in rows if r.get("result") == "PASS" and "stage" not in r
    ]
    transfer = [r for r in rows if r.get("stage") == "nonlocal-transfer"]
    fpe = [r for r in rows if r.get("stage") == "guest-fpe"]
    fpe_recipient = [r.get("recipient") for r in fpe]
    fpe_sources = [r.get("source") for r in fpe]
    fpe_valid = fpe_recipient == ["carrier", "pthread"] and all(
        r.get("recovered") is True
        and (
            r.get("source") == "hardware"
            or (fpe_optional and r.get("source") == "kernel-self")
        )
        for r in fpe
    )
    vm_samples = [r for r in rows if r.get("stage") == "vm-suspend-and-sample"]
    fault_masks = [r for r in rows if r.get("stage") == "host-fault-mask"]
    guest_fault_masks = [
        r for r in rows if r.get("stage") == "guest-fault-mask"
    ]
    warnings = (
        "WARNING in native method" in output
        or "Consider using jsig" in output
        or "Calling other JNI functions in the scope of" in output
    )
    failed = (
        timed_out
        or returncode != 0
        or len(terminal) != 1
        or malformed
        or warnings
        or (
            mode
            in (
                "default",
                "worker",
                "borrowed-relay",
                "worker-borrowed-relay",
                "worker-signals",
                "adopted-signals",
                "vm-signals",
                "nonlocal",
            )
            and not valid_relay_lifecycle(rows, mode)
        )
        or (mode == "adopted-signals" and not valid_jni_signal_recovery(rows))
        or (
            mode == "adopted-signals"
            and [r for r in rows if r.get("stage") == "adopted-signal"]
            != [
                {
                    "stage": "adopted-signal",
                    "mode": phase,
                    "same_tid": True,
                    "owned_context": True,
                    "post_signal_jni": True,
                }
                for phase in range(21)
                if phase != 17 or not require_host_private
            ]
        )
        or (
            mode == "adopted-signals"
            and [r for r in rows if r.get("stage") == "adopted-scope-resume"]
            != [
                {
                    "stage": "adopted-scope-resume",
                    "round_trips": 3,
                    "host_reentries": 3,
                    "host_returned": True,
                }
            ]
        )
        or (
            mode == "adopted-signals"
            and [r for r in rows if r.get("stage") == "adopted-scope-overlap"]
            != [{"stage": "adopted-scope-overlap", "rejected": True}]
        )
        or (
            mode == "adopted-signals"
            and [r for r in rows if r.get("stage") == "adopted-scope-retire"]
            != [
                {
                    "stage": "adopted-scope-retire",
                    "exit": exit_kind,
                    "stale_rejected": retired,
                    "host_callbacks_retired": retired,
                }
                for exit_kind, retired in (
                    ("return", 1),
                    ("host-jump", 1),
                    ("return", 2),
                )
            ]
        )
        or (
            mode == "vm-signals"
            and (
                len(vm_samples) != 1
                or vm_samples[0].get("parked_frames") != 2
                or vm_samples[0].get("jni_return_suspended") is not True
                or vm_samples[0].get("stacks_released") is not True
                or vm_samples[0].get("native_samples", 0) < 1
                or not (
                    0
                    < vm_samples[0].get("java_samples", 0)
                    <= vm_samples[0].get("sr_deliveries", 0)
                )
            )
        )
        or (
            mode in ("adopted-signals", "worker-signals")
            and [r for r in rows if r.get("stage") == "guest-signalfd"]
            != [
                {
                    "stage": "guest-signalfd",
                    "aliases": True,
                    "copyout": True,
                    "vectors": True,
                    "mask_update": True,
                    "interruption": True,
                    "restart": True,
                    "cancellation": True,
                    "stdio": True,
                    "stdio_nocancel": True,
                    "stdio_close": True,
                }
            ]
        )
        or (
            mode in ("adopted-signals", "worker-signals")
            and [r for r in rows if r.get("stage") == "guest-signalfd-combined"]
            != [
                {
                    "stage": "guest-signalfd-combined",
                    "gnu_syscall": True,
                    "restart_action_snapshot": True,
                    "stale_notification": True,
                    "shared_job_mask": True,
                    "native_fds": True,
                }
            ]
        )
        or (
            mode in ("adopted-signals", "worker-signals")
            and [r for r in rows if r.get("stage") == "host-guest-signalfd"]
            != [
                {
                    "stage": "host-guest-signalfd",
                    "directed_owner": True,
                    "shared_wait": True,
                    "close_wake": True,
                    "blocked_cleanup": True,
                    "stdio": mode == "adopted-signals",
                }
            ]
        )
        or (
            mode in ("adopted-signals", "worker-signals")
            and [r for r in rows if r.get("stage") == "guest-job-control"]
            != [
                {
                    "stage": "guest-job-control",
                    "native_stops": 3,
                    "resumed": True,
                    "host_disposition": True,
                }
            ]
        )
        or (
            mode in ("adopted-signals", "worker-signals")
            and [r for r in rows if r.get("stage") == "guest-signalfd-ready"]
            != [
                {
                    "stage": "guest-signalfd-ready",
                    "poll_select": True,
                    "epoll_modes": True,
                    "nested": True,
                    "separate_targets": True,
                    "concurrent_close": True,
                    "cancellation": True,
                    "temporary_masks": True,
                    "abandoned_waits": True,
                }
            ]
        )
        or (
            mode == "worker-signals"
            and [r for r in rows if r.get("stage") == "worker-signals"]
            != [
                {
                    "stage": "worker-signals",
                    "native_children": 32,
                    "cancelled_children": 64,
                    "alternate_stack": True,
                    "one_shot": True,
                    "consuming_waits": True,
                    "host_parallel_disposition": True,
                    "native_cancel": True,
                }
            ]
        )
        or (
            require_host_fault_mask
            and (
                fault_masks
                != [
                    {"stage": "host-fault-mask", "armed": True},
                    {
                        "stage": "host-fault-mask",
                        "completed": True,
                        "value": 73,
                    },
                ]
                or len(guest_fault_masks) != 14
                or any(
                    r.get("signal") != int(signal.SIGTRAP)
                    or r.get("logical_blocked") is not False
                    or r.get("physical_blocked") is not False
                    for r in guest_fault_masks
                )
                or [r for r in rows if r.get("stage") == "host-fault-deferred"]
                != [
                    {
                        "stage": "host-fault-deferred",
                        "deliveries": 1,
                        "first_siginfo": True,
                        "logical_pending": True,
                        "consuming_waits": 3,
                        "retirement_busy": True,
                        "retirement_retry": True,
                    }
                ]
                or not isinstance(
                    guest_fault_masks[0].get("physical_blocked"), bool
                )
                or [r for r in rows if r.get("stage") == "host-signalfd"]
                != [
                    {
                        "stage": "host-signalfd",
                        "read": True,
                        "readv": True,
                        "readiness": True,
                        "metadata": True,
                        "alias": True,
                        "guest_return": True,
                    }
                ]
                or [r for r in rows if r.get("stage") == "host-signalfd-unwind"]
                != [
                    {
                        "stage": "host-signalfd-unwind",
                        "read": True,
                        "readv": True,
                        "context_release": True,
                        "jni_retry": True,
                    }
                ]
                or [r for r in rows if r.get("stage") == "host-signalfd-stdio"]
                != [
                    {
                        "stage": "host-signalfd-stdio",
                        "native_file": True,
                        "metadata": True,
                        "fileno": True,
                        "freopen": True,
                        "close": True,
                        "jni_retry": True,
                    }
                ]
                or [r for r in rows if r.get("stage") == "host-ignore-pending"]
                != [
                    {
                        "stage": "host-ignore-pending",
                        "discarded": True,
                        "guest_trap": True,
                        "fresh_read": True,
                        "context_release": True,
                        "jni_retry": True,
                    }
                ]
            )
            and fatal is None
        )
        or (
            mode == "nonlocal"
            and (
                len(transfer) != 1
                or [r for r in rows if r.get("stage") == "gnu-host-relay"]
                != [
                    {
                        "stage": "gnu-host-relay",
                        "threads": 8,
                        "deliveries": 64,
                        "host_tls": True,
                        "guest_tls_errno": True,
                        "original_siginfo": True,
                        "different_thread": True,
                        "source_context_edits": True,
                    }
                ]
                or not fpe_valid
                or [r for r in rows if r.get("stage") == "host-async-fault"]
                != [
                    {
                        "stage": "host-async-fault",
                        "signal": int(signal.SIGSEGV),
                        "delivered": True,
                        "metadata": True,
                    }
                ]
                or [
                    r
                    for r in rows
                    if r.get("stage") == "host-unreserved-signal"
                ]
                != [
                    {
                        "stage": "host-unreserved-signal",
                        "signal": int(signal.SIGUSR1),
                        "host_tls": True,
                        "metadata": True,
                        "guest_return": True,
                        "exclusive": False,
                    }
                ]
                or not transfer[0].get("cooperative_masks")
                or not transfer[0].get("handler_mask")
                or not transfer[0].get("reserved_masks")
                or not transfer[0].get("context_masks")
                or transfer[0].get("guest_faults") != 15
                or transfer[0].get("guest_fault_threads") != 8
                or transfer[0].get("guest_signal_stacks") != 19
                or transfer[0].get("stack_unwinds") != 8
                or transfer[0].get("host_signal_numbers") != 2
                or transfer[0].get("guest_wait_cases") != 19
                or transfer[0].get("guest_registration_aliases") != 9
                or not valid_registration_observations(rows)
                or not valid_signal_unwind_observations(rows)
                or transfer[0].get("wait_unwinds") != 2
                or not valid_wait_observations(rows, transfer[0])
                or (
                    require_guest_target
                    and (
                        not valid_guest_target_observations(rows)
                        or not valid_jni_resource_recovery(rows)
                    )
                )
                or (require_pending and not transfer[0].get("kernel_pending"))
            )
        )
    )

    if mode == "router-quarantine":
        failed = (
            timed_out
            or returncode != 0
            or malformed
            or warnings
            or terminal
            != [
                {
                    "result": "PASS",
                    "case": mode,
                    "warmup": True,
                    "admission_rejected": True,
                    "close_rejected": True,
                    "shutdown_rejected": True,
                    "resources_retained": True,
                }
            ]
        )

    if fatal is not None:
        armed = [r for r in rows if r.get("stage") == "guest-default"]
        executed = [
            r for r in rows if r.get("stage") == "guest-default-execute"
        ]
        # adb reports the remote shell's 128+signal; a direct subprocess uses
        # -signal. An arbitrary nonzero exit (including our fail-closed codes)
        # is never proof of the owning libc's default signal action.
        failed = (
            timed_out
            or malformed
            or warnings
            or returncode not in (-signal.SIGTRAP, 128 + signal.SIGTRAP)
            or len(armed) != 1
            or len(executed) != 1
            or armed[0].get("mode") != fatal
            or executed[0].get("mode") != (33 if fatal == 35 else fatal)
            or not armed[0].get("tid")
            or not executed[0].get("tid")
            or (
                (armed[0].get("tid") != executed[0].get("tid")) != (fatal == 35)
            )
        )
        terminal = [
            {"case": mode, "signal": int(signal.SIGTRAP), "result": "PASS"}
        ]

    if require_host_private:
        expected = (
            []
            if mode == "unconfigured-profile"
            else [
                {
                    "stage": "bionic-private-registration",
                    "result": "PASS",
                    "vm_running": True,
                    "host_tls": True,
                }
            ]
        )
        failed |= [
            r for r in rows if r.get("stage") == "bionic-private-registration"
        ] != expected

    evidence = {
        "case": mode,
        "returncode": returncode,
        "timeout": timed_out,
        "observations": rows,
        "command": command,
        "result": "FAIL" if failed else "PASS",
    }
    (out / (mode + ".json")).write_text(json.dumps(evidence, indent=2) + "\n")
    if failed:
        (out / "failure.json").write_text(json.dumps(evidence, indent=2) + "\n")
        raise SystemExit(
            "Runtime SDK case failed: " + str(out / (mode + ".log"))
        )

    summary = {"case": mode, **terminal[0]}
    if mode == "adopted-signals":
        summary["jni_signal_recoveries"] = sum(
            r.get("stage") == "jni-resource-signal" for r in rows
        )
    if mode == "vm-signals":
        summary.update(vm_samples[0])
        del summary["stage"]
    if mode == "nonlocal":
        skipped = transfer[0]["guest_wait_skipped"]
        summary.update(
            guest_fpe=dict(zip(fpe_recipient, fpe_sources)),
            wait_exercised=transfer[0]["guest_wait_cases"] - skipped,
            wait_skipped=skipped,
            wait_unwinds=transfer[0]["wait_unwinds"],
            guest_signal_unwinds=4,
        )
        if require_guest_target:
            summary.update(
                jni_host_recoveries=sum(
                    r.get("stage") == "jni-resource-host" for r in rows
                ),
                jni_guest_recoveries=sum(
                    r.get("stage") == "jni-resource-guest" for r in rows
                ),
                jni_managed_boundary=True,
            )

    return summary
