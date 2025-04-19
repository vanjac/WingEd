# WingEd

WingEd is a (work in progress) 3D modeling tool focused on game dev and low-poly level design / prototyping. Its interface is inspired by SketchUp, [Hammer 2](https://developer.valvesoftware.com/wiki/Source_2), and [Doom Builder](https://doomwiki.org/wiki/Doom_Builder_2).

<img src="http://chroma.zone/img/winged-demo.webp" width="542" alt="Animated demo of WingEd">

WingEd works on Windows only. [Download latest build](https://github.com/vanjac/WingEd/releases/latest/download/winged.exe) (unstable, for testing only!)

## Using WingEd

See [WingEd interface documentation](http://chroma.zone/winged/docs/interface)

## Technical Details

WingEd only operates on closed [manifold](https://en.wikipedia.org/wiki/Surface_(topology)) surfaces. Internally it uses the [half-edge](https://en.wikipedia.org/wiki/Doubly_connected_edge_list) data structure to represent meshes.

WingEd makes use of [persistent data structures](https://en.wikipedia.org/wiki/Persistent_data_structure) to store editor state, using the [immer](https://github.com/arximboldi/immer) library. This allows for simple and robust implementation of undo, and rollback after errors. In the future it could help with adding concurrency.
