# ncp

New CP.

The venerable `cp` tool is powerful and well optimized(well, usually), but it's got a lot of semantics that
don't usually align with my personal preferences. `cp` doesn't provide progress reports, `cp` works on individual files rather than directories.

Worst of all, `cp` doesn't show a rapidly updating number for maximum instant gratification(this has minimal effect on write efficiency on my system).

C++ makes it easy to replicate the parts we care about in
what is likely far less code(looking at coreutils code is a penance
when you don't care about the standard tool functionality)

It cares about efficiency, it uses `mmap` when `sendfile`
isn't available, but prefers that when possible because it's faster.

I don't recommend it for you because this was born of a fit of pique.


## Syntax and Options

The general format is `ncp [options] (origin directory) (destination directory)` There are a few options that you might find useful.

* `--loud` Print messages. The default
* `--silent` Do not print messages or progress.
* `--chunk size <N>` Set the size of chunks copied, accepts suffixed numbers with `k`,`m`,`g` for kilobytes, megabytes, and gigabytes respectively.
* `--preserve-permissions` Copy the permissions of the original file. On by default.
* `--disregard-permissions` Do not copy the permissions of the original file. This is probably not useful.