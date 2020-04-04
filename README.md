# ncp

New CP.

The venerable `cp` tool is powerful and well optimized(well, usually), but it's got a lot of semantics that
don't usually align with our personal preferences. `cp` doesn't provide progress reports, `cp` works on individual files rather than directories.

Worst of all, `cp` doesn't show a quickly updating number for maximum instant gratification.

C++ makes it easy to replicate the parts we care about in
what is likely far less code(looking at coreutils code is a penance
when you don't care about the standard tool functionality)

It cares about efficiency, it uses `mmap` when `sendfile`
isn't available, but prefers that when possible because it's faster.

It wrongly assumes you know what you're doing and doesn't bother catching
exceptions for permission issues and other such nonsense.

We don't recommend it for you because this was born of a fit of pique.
But if you think posix is encumbered with too much unix interop design
and you hate unix and think that robocopy is still too complicated, this... might be better?