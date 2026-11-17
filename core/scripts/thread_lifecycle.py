"""Add optional pre-clone admission and final parent completion to NPTL."""

import shutil


def patch_thread_lifecycle(src, fragments, replace):
    replace(
        src / "nptl/descr.h",
        "  bool gw_mask_starting;",
        "  bool gw_mask_starting;\n"
        "  void *gw_create_cookie;\n"
        "  bool gw_create_prepared;",
    )
    shutil.copyfile(
        fragments / "thread-create.h", src / "include/gw-thread-create.h"
    )
    shutil.copyfile(
        fragments / "thread-create.inc", src / "nptl/gw-thread-create.inc"
    )
    create = src / "nptl/pthread_create.c"
    replace(
        create,
        "static int create_thread (struct pthread *pd, const struct pthread_attr *attr,\n"
        "\t\t\t  bool *stopped_start, void *stackaddr,\n"
        "\t\t\t  size_t stacksize, bool *thread_ran)",
        "static int gw_create_native (struct pthread *pd, const struct pthread_attr *attr,\n"
        "\t\t\t  bool *stopped_start, void *stackaddr,\n"
        "\t\t\t  size_t stacksize, bool *thread_ran, int *gw_tid)",
    )
    replace(
        create,
        "  *thread_ran = true;",
        "  *gw_tid = ret;\n  *thread_ran = true;",
    )
    replace(
        create,
        "/* Local function to start thread and handle cleanup.  */",
        '#include "gw-thread-create.inc"\n\n'
        "/* Local function to start thread and handle cleanup.  */",
    )
    replace(
        create,
        "  if (gw_thread_hook) gw_thread_hook (1);",
        "  if (gw_thread_hook) gw_thread_hook (1);\n  gw_create_event (pd, 1);",
    )
    replace(
        create,
        "  if (gw_thread_hook) gw_thread_hook (0);",
        "  gw_create_event (pd, 0);\n  if (gw_thread_hook) gw_thread_hook (0);",
    )
    replace(
        src / "nptl/Versions",
        "__gw_domain_thread_hook;",
        "__gw_domain_thread_hook; __gw_domain_thread_create_hook;",
    )
