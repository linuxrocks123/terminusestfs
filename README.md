# terminusestfs
The Terminus Est Filesystem.

What this basically is is a caching filesystem that lets you write to a fast/local storage area ("upper layer") and send these writes to a slow/remote storage area ("lower layer") over time.

One-way mode should work okay, but this is alpha/experimental/mostly undocumented.  design.txt does contain some documentation if you'd like to sort through it.

Two-way mode will allow this to be used as a full distributed filesystem, but two-way mode is COMPLETELY untested at the moment.

You must run this in foreground mode.  Invoke like this:

~~~~
./terminusestfs -f upper_layer lower_layer mountpoint
~~~~

This is alpha software: back up your stuff if you use this.  If you use this for anything important and don't have backups, it's your funeral.
