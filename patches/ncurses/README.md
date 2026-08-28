# ncurses patches

`0001-config-sub-extron.patch` is the maintained Extron delta against ncurses
6.4. It teaches the bundled GNU configuration triplet parser to recognize the
`extron` operating-system component.

The build helper accepts both pristine and already-patched source trees, making
repeated local builds safe.
