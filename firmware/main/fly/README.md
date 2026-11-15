# The fly eye

This folder contains a small model of the motion pathway in the eye of the fruit fly. The firmware uses it to find
what moves in the camera picture. It operates together with the decision model. The file `../eye.cc` connects it to
the camera, the rules and the page.

| File | Contents |
|---|---|
| `fly_eye.h` | The optic lobe: a 40x30 retina from the 160x120 frame, contrast and local motion (flow). It also has the wide-field cells that sense the motion of the camera itself |
| `fly_figure.h` | Figure and ground: the motion that the motion of the camera does not explain. It puts this motion into figures and follows the figures as tracks. It also keeps a background in memory and finds what stays different |
| `fly_lock.h` | One selected figure, which it follows by its appearance |
| `fly_detectors.h` | The events: new object, object left, approach, room changed |
| `fly_vision.h` | The frame for the files above: one percept for each frame, a list of detectors |
| `fly_core.h` | The `Fly` class: a small associative memory for habituation (after some time, a fan is not a new event) |
| `fly_ear.h` | The sound side. It is in the source, but the firmware does not use it yet |
| `circuit_data.h`, `optic_lobe_data.h` | Connection tables between cell types |

These files come from the fruit-fly firmware of the author. The author made them smaller for a fixed camera. They do
not have flight control, time-to-contact reflexes, a world compass, an item recogniser or a radio link. The decision
model does the task of the item recogniser. All files are plain C++, and they do not depend on a board.

## Limits

- It sees movement, not objects. It cannot tell what a figure is.
- An object that does not move goes into the background after a few seconds.
- The author adjusted it at 18 to 23 frames each second, with its arrays in fast memory. Here it operates at 12 to 20
  frames each second from PSRAM. Its thresholds were not adjusted again.
- When the full picture moves (a person carries the board or hits it), the eye does not report figures.

## Data

The data in `circuit_data.h` and `optic_lobe_data.h` comes from the male-cns v1.0 connectome of *Drosophila
melanogaster* (Janelia FlyEM Project and the University of Cambridge). The licence of the connectome is CC BY 4.0.
If you give these files to other persons, give credit to the Janelia FlyEM Project and the University of Cambridge.
