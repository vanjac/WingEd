# WingEd

WingEd is a (work in progress) 3D modeling tool focused on game dev and low-poly level design. Its interface is inspired by SketchUp, [Hammer 2](https://developer.valvesoftware.com/wiki/Source_2), and [Doom Builder](https://doomwiki.org/wiki/Doom_Builder_2).

WingEd works on Windows only (XP through 11).

<img src="https://github.com/vanjac/WingEd/assets/8228102/8aef6f57-1027-47de-9005-ca4e96f22e41" width="439" alt="Screenshot of WingEd window">

[Download latest build](https://github.com/vanjac/WingEd/releases/latest/download/winged.exe). Currently incomplete and unstable, don't use it for anything important :)

## Technical Details

WingEd only operates on closed [manifold](https://en.wikipedia.org/wiki/Surface_(topology)) surfaces. Internally it uses the [half-edge](https://en.wikipedia.org/wiki/Doubly_connected_edge_list) data structure to represent meshes.

WingEd makes use of [persistent data structures](https://en.wikipedia.org/wiki/Persistent_data_structure) to store editor state, using the [immer](https://github.com/arximboldi/immer) library. This allows for simple and robust implementation of undo, and rollback after errors. In the future it could help with adding concurrency.
