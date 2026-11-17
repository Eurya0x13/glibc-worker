"""Preserve logical masks separately from native NPTL startup masks."""


def patch_mask_lifecycle(src, replace):
    replace(
        src / "nptl/descr.h",
        "  internal_sigset_t sigmask;",
        "  internal_sigset_t sigmask;\n"
        "  uint64_t gw_inherited_mask;\n"
        "  uint64_t gw_inherited_carrier;\n"
        "  bool gw_inherited_mask_valid;\n"
        "  bool gw_mask_starting;",
    )
    replace(
        src / "sysdeps/nptl/pthreadP.h",
        "libc_hidden_proto (__pthread_sigmask);",
        "libc_hidden_proto (__pthread_sigmask);\n"
        "extern void __gw_domain_mask_inherit (internal_sigset_t *,\n"
        "                                       uint64_t *, int) attribute_hidden;",
    )
    replace(
        src / "nptl/pthread_create.c",
        "  /* Start the thread.  */",
        "  /* Logical inheritance must be published before clone, including\n"
        "     an explicit GNU attribute mask. The child owns its GNU thread;\n"
        "     a child runtime binding can then prepare its private wake bits. */\n"
        "  __gw_domain_mask_inherit (&pd->sigmask, &pd->gw_inherited_mask,\n"
        "      iattr->extension != NULL && iattr->extension->sigmask_set);\n"
        "  memcpy (&pd->gw_inherited_carrier, &pd->sigmask,\n"
        "          sizeof pd->gw_inherited_carrier);\n"
        "  pd->gw_inherited_mask_valid = true;\n"
        "  pd->gw_mask_starting = true;\n\n"
        "  /* Start the thread.  */",
    )
    replace(
        src / "nptl/pthread_create.c",
        "      internal_signal_restore_set (&pd->sigmask);",
        "      pd->gw_mask_starting = false;\n"
        "      internal_signal_restore_set (&pd->sigmask);",
    )
    replace(
        src / "nptl/Versions",
        "__gw_domain_mask_bind;",
        "__gw_domain_mask_bind; __gw_domain_mask_hook;\n"
        "    __gw_domain_mask_inherited;",
    )
