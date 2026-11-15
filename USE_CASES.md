# What to build with decidio

This file lists 111 ideas for a small decision model on an ESP32-S3 board. For each idea, the tables tell you which
part of decidio it uses. The questions of the base models were selected from this list. When a question is useful
for more ideas, there is more reason for the base model to answer it.

How to read the **Uses** column:

| Value | Meaning |
|---|---|
| **base** | A base model answers the question. The **Install** column gives the name of a ready-made build. Install it with `decidio install <name> --port COMx`, or select it on the web page of the board (Ideas tab) |
| **teach** | Add your own question to a base model with `decidio teach` and a few tens of your own examples. For pictures and sounds that are very different from usual photos and sounds, a fine-tune (`decidio run`) is necessary. Refer to GUIDE.md |
| **sensor** | The idea uses an input that the model does not have. The model uses only a camera and a microphone. Refer to [skills/add-sensor](skills/add-sensor/SKILL.md) |
| **eye** | The idea uses the fly eye, which finds the objects that move in the camera picture (ideas 106 to 111) |
| **radio** | The idea uses the Wi-Fi signal in place of the model. Three movement builds exist (ideas 81 and 90). The other radio rows are only ideas |

A name with "(teach)" after it is not a build. The file `recipes/teach.yaml` tells you what to record for that idea.

GUIDE.md gives the measured accuracy of each base answer. Each row is a start point, not a finished product. Do a
test of each idea in your own room.

**WARNING: Do not let the safety of a person depend on an idea. The model makes errors.**

## In the home (a board in a fixed position)

| # | Idea | Uses | Install |
|---|---|---|---|
| 1 | Lights on while a person is in the room, also when the person does not move | base: `person_present` | `lights-while-occupied` |
| 2 | Switch the TV off when no person watches it | base: `person_present` + `subject = electronics` | `tv-off-unwatched` |
| 3 | The desk lamp and the monitor switch on when you sit at the desk | base: `person_present` | `desk-wake` |
| 4 | Front door: a person stays at the door and does not only go past | base: `person_present` with a long `for_ms` | `door-lingering` |
| 5 | A car in the driveway, or a parking space that is in use | base: `vehicle_present` | `car-in-driveway` |
| 6 | A pet at the door: open the pet door or send an alert | base: `animal_type` | `pet-at-door` |
| 7 | A bird at the feeder: take a photo, count the visits | base: `animal_type = bird` | `bird-at-feeder` |
| 8 | Wild animals or pests in the garden: start a water sprinkler | base: `animal_present` | `garden-animal` |
| 9 | The hens are back in the coop in the evening: close the door | base: `animal_present` | `hens-in-coop` |
| 10 | A knock on the door: make the lights flash (for a deaf person, or for a person with headphones) | base: `home_sound = knock` | `knock` |
| 11 | Glass breaks: start an alarm | base: `home_sound = glass` | `glass-break` |
| 12 | A baby cries: send an alert | base: `home_sound = baby` | `baby-crying` |
| 13 | A dog barks while you are not at home: record it in a log | base: `home_sound = dog` | `dog-barking` |
| 14 | Water flows from a tap or a toilet for a long time | base: `home_sound = water` for minutes | `water-left-running` |
| 15 | The washing machine is at the end of its cycle | base: `home_sound = appliance` stops | `washer-finished` |
| 16 | A log of coughs and snores at night | base: `home_sound` | `cough-snore-log` |
| 17 | A noise monitor for quiet hours | base: `sound_group` is not `quiet` | `quiet-hours-noise` |
| 18 | A "Room in use" sign that comes on when persons speak. No sound leaves the board | base: `speech_present` | `room-in-use` |
| 19 | Rain starts: close the roof window, bring the clothes inside | base: `weather = rain`, `weather_sound = rain` | `rain-started` |
| 20 | Stop the irrigation when it rains | base: `weather` | `skip-irrigation` |
| 21 | Snow on the driveway: start a heater or send an alert | base: `weather = snow` | `snow-on-driveway` |
| 22 | Fog: switch on the outdoor lights | base: `weather = fog` | `fog-lights` |
| 23 | Lower the blinds when the sun shines, raise them when the sky has clouds | base: `weather` | `blinds-by-sky` |
| 24 | Thunder: send a warning, disconnect devices from the power supply | base: `weather_sound = thunder` | `thunder` |
| 25 | The wind becomes stronger: retract the awning | base: `weather_sound = wind` | `wind-picking-up` |
| 26 | No person in the kitchen before 10:00 (a check on a person who lives alone) | base: `person_present` + `place` | `nobody-in-kitchen` |
| 27 | A device that knows which room it is in | base: `place` | `which-room` |
| 28 | A smoke alarm gives its beep signal: send an alert to a phone | teach (sound) | `smoke-alarm` (teach) |
| 29 | A beep from a doorbell, a kettle or a microwave oven | teach (sound) | `appliance-beep` (teach) |
| 30 | Music plays: set the lights for music | teach (sound) | `music-playing` (teach) |
| 31 | A parcel on the doorstep | teach | `parcel-on-doorstep` (teach) |
| 32 | Mail in the mailbox (the camera is in the mailbox) | teach | `mail-in-mailbox` (teach) |
| 33 | The garage door is open or closed | teach | `garage-door` (teach) |
| 34 | A window or a door stays open | teach | `window-left-open` (teach) |
| 35 | The door of the fridge stays open | teach | `fridge-door-open` (teach) |
| 36 | The stove is on and no person is in the kitchen | teach + `person_present` | `stove-unattended` (teach) |
| 37 | A candle or a fireplace burns and no person is near it | teach + `person_present` | `candle-unattended` (teach) |
| 38 | A pet on the couch or on the counter | teach | `pet-on-the-couch` (teach) |
| 39 | The bowl of the pet is empty | teach | `pet-bowl-empty` (teach) |
| 40 | The bins are outside on the day of the collection | teach | `bins-out` (teach) |
| 41 | A plant that wilts | teach | `plant-wilting` (teach) |
| 42 | A 3D print that failed | teach | `print-failed` (teach) |
| 43 | Water on the floor below the sink | teach | `puddle-under-sink` (teach) |
| 44 | The sound of a heavy impact, then silence (possibly a fall) | teach (sound) | `thud-then-silence` (teach) |

## Carried: in a pocket, in a bag, on a bike or on the body

| # | Idea | Uses | Install |
|---|---|---|---|
| 45 | A siren or a horn: warn a person who wears headphones | base: `alert_sound` | `siren-or-horn` |
| 46 | A rear camera on a bike: a vehicle is near behind you, flash the light | base: `vehicle_present` | `bike-vehicle-behind` |
| 47 | A camera on a backpack: a person is near behind you | base: `person_present` | `someone-behind` |
| 48 | An aid for a person with low vision: a person or a vehicle in front, give a vibration | base: `person_present`, `vehicle_present` | `person-or-vehicle-ahead` |
| 49 | Indoors or outdoors: change the profile of the phone | base: `indoors` | `indoors-or-outdoors` |
| 50 | A reminder to take an umbrella when you go outside | base: `indoors` + `weather` | `umbrella-reminder` |
| 51 | Tags for a life log: the place, the weather, what is in front | base: `place`, `weather`, `subject` | `life-log` |
| 52 | Pause the music when a person speaks to you | base: `speech_present` | `pause-on-speech` |
| 53 | A counter of conversation time that records nothing | base: `speech_present` | `conversation-time` |
| 54 | A counter of coughs and sneezes | base: `home_sound = cough` | `cough-counter` |
| 55 | Clap your hands to start an action | base: `home_sound = clap` | `clap-switch` |
| 56 | A spoken command or a wake word | teach (the base model was trained to identify 35 different words) | `wake-word` (teach) |
| 57 | Know when you travel to work, from the sounds of engines and trains | base: `sound_group = outside` | `commute` |
| 58 | A log of noise exposure near loud machines | base: `sound_group = outside` | `noise-exposure` |
| 59 | A counter of screen time (a screen in view) | base: `subject = electronics` | `screen-time` |
| 60 | A meal log (food in view) | base: `subject = food` | `meal-log` |
| 61 | On a hike: an animal in front of you | base: `animal_present` | `hiking-animal-ahead` |
| 62 | Start the camera when a bird or an animal is in the picture | base: `animal_type` | `animal-camera-trigger` |
| 63 | A bus comes to the stop | base: `vehicle_type = bus` | `bus-arriving` |
| 64 | Time-lapse photos only when the sky is clear | base: `weather = clear` | `clear-sky-timelapse` |
| 65 | A person says your name while you wear headphones | teach (sound) | `name-called` (teach) |
| 66 | The keys, the wallet and the bag are in view when you go out | teach | `keys-in-view` (teach) |
| 67 | The traffic light for pedestrians is red or green | teach | `crossing-light` (teach) |
| 68 | A hand sign in front of the camera | teach | `hand-sign` (teach) |
| 69 | You read, look at a screen or look far away (a timer for eye strain) | teach | `eye-strain-timer` (teach) |
| 70 | Fall detection, a step counter, the type of activity, a wand for gestures | sensor (accelerometer) | – |
| 71 | Weather from pressure, temperature and humidity | sensor | – |

## General ESP32 projects

| # | Idea | Uses | Install |
|---|---|---|---|
| 72 | A camera on a battery that switches on Wi-Fi only for an important event | base: any answer | `wake-on-event` |
| 73 | A trail camera that keeps only the frames with animals | base: `animal_present` | `trail-camera` |
| 74 | Room occupancy for the heating and the ventilation | base: `person_present` | `occupancy-hvac` |
| 75 | A robot that follows persons or keeps away from them | base: `person_present` | `robot-follow-people` |
| 76 | A toy that reacts to persons, claps and voices | base | `reactive-toy` |
| 77 | An empty shelf, a queue, an item on the conveyor | teach | `shelf-queue-conveyor` (teach) |
| 78 | A person wears a helmet or a high-visibility vest | teach | `helmet-worn` (teach) |
| 79 | A machine that makes an incorrect sound (a bearing, a pump) | teach (sound), probably with a fine-tune | `machine-sounds-wrong` (teach) |
| 80 | The condition of a beehive, from its sound | teach (sound), probably with a fine-tune | `beehive-state` (teach) |

## Without a camera: radio and sound

Many of the ideas above can operate without a camera. Two other senses cost less: a microphone, and the radio of the
ESP32. The Wi-Fi channel state (CSI) and the signal strength (RSSI) change when a body moves through the path between
two devices. Bluetooth scans and Wi-Fi scans show which devices are near.

**radio** in the Uses column is an input that the model does not have. One part of it exists. When the board is
connected to your Wi-Fi, it calculates a score for the changes in the signal of the router. The score increases when
a person walks between the router and the board. A rule uses the score as `radio_motion`, and the Radio tab of the
page of the board shows it live.

A test of two minutes in one room had this result. The score always made a distinction between a person who walks
and a person who sits without movement. It could not make a distinction between a person who sits without movement
and an empty room. Thus the movement builds (81, 90) are only for movement. You must set their threshold in your room.

All other radio rows are only ideas. A radio pattern is specific to one room and to one layout of devices. Thus, to
make one of these ideas, a model must learn from a few minutes of signal that you record in that room. You can
record the signal with the `radio` commands and `tools/radio_record.py` (refer to GUIDE.md).

| # | Idea | Uses | Install |
|---|---|---|---|
| 81 | The room is occupied, from the Wi-Fi link between the board and the router | radio (CSI) | `radio-room-movement` |
| 82 | A person sits without movement or sleeps in the room (the breath moves the signal) | radio (CSI) | – |
| 83 | The direction in which a person went through a doorway, with two links | radio (CSI) | – |
| 84 | No person, one person or more persons in the room | radio (CSI) | – |
| 85 | A fall: a sudden movement, then no movement | radio (CSI) | – |
| 86 | The breath rate near the bed | radio (CSI) | – |
| 87 | A wave or a push of the hand near the device as a switch | radio (CSI) | – |
| 88 | A door or a window is open or closed, from the change in the pattern when no person moves | radio (CSI) | – |
| 89 | A bed or a chair is occupied, with a link across it | radio (CSI or RSSI) | – |
| 90 | Movement in the house while all known phones are away | radio (CSI + scan) | `radio-movement-alert`, `radio-movement-night` |
| 91 | A pet or a person, from the size and the speed of the change in the signal | radio (CSI) | – |
| 92 | A car in the garage (a large metal object changes the pattern) | radio (CSI) | – |
| 93 | At home or away, from the Bluetooth signal of your phone | radio (scan) | – |
| 94 | The room where a carried tag is, from the signal strength at fixed boards | radio (RSSI) | – |
| 95 | An alert for an item that you left behind: the signal of the tag on your bag or keys becomes weak | radio (RSSI) | – |
| 96 | How many persons are at a place, from the number of devices that send advertisements | radio (scan) | – |
| 97 | The same unknown tag is near you at more than one place (a tracker that follows you) | radio (scan) | – |
| 98 | The room is occupied, from sound only: steps, speech, movement | base: `speech_present`, `home_sound`, `sound_group` | `occupied-by-sound` |
| 99 | A person is at the desk, from the sound of the keyboard | teach (sound; the base model was trained on keyboard sounds) | `typing-at-desk` (teach) |
| 100 | A door opened or closed, from its sound | teach (sound) | `door-by-sound` (teach) |
| 101 | A pattern of knocks or taps on the table as a switch | teach (sound) | `knock-pattern` (teach) |
| 102 | A timer for the shower or the tap | base: `home_sound = water` | `shower-timer` |
| 103 | How much rain falls on the roof or the window | base: `weather_sound = rain`; teach for levels | `rain-on-the-roof` |
| 104 | The fridge, the heating system or a pump operates, and operates too frequently | teach (sound) | `fridge-or-pump-running` (teach) |
| 105 | A pan boils over, a kettle boils | teach (sound) | `boiling-over` (teach) |

Power: a microphone uses much less power than the other senses. To receive Wi-Fi signals, the radio must stay on.
This uses approximately the same power as the camera. Thus the radio sense decreases the power only if the board
receives in short periods. The radio has these advantages: it senses through walls and furniture and in the dark, and
it cannot take a picture.

## With the fly eye: what moves and where it is

The firmware has a second method to examine the camera picture: the fly eye (the Eye tab on the page of the board).
It operates together with the model. It examines the full camera frame as 40 x 30 points, approximately 15 times
each second, and finds the objects that move against the background. It does not know what an object is. It knows that an object
moves, where it is, if it comes nearer, and if the view stays different after the movement stops. A rule can use the
eye alone, or the eye together with a model answer.

**eye** in the Uses column is one of the numbers of the eye. The file `recipes/eye.yaml` lists them. The eye
operates only with a camera. It sees only movement: a person who stands without movement goes out of its view after
a few seconds. The thresholds in these builds are start values. They were not adjusted with persons in front of the
boards.

| # | Idea | Uses | Install |
|---|---|---|---|
| 106 | An object of any type moves in view | eye: `eye_moving` | `anything-moves` |
| 107 | An object moves and it is not a person (a pet, a robot vacuum cleaner) | eye: `eye_moving`, and base `person_present` | `moves-but-no-person` |
| 108 | A person who moves, not a photo or a poster of a person | eye: `eye_moving`, and base `person_present` | `person-moving` |
| 109 | An object comes toward the camera | eye: `eye_approach` | `something-approaches` |
| 110 | An object is in a new position, is new, or is not there now | eye: `eye_changed` | `scene-changed` |
| 111 | The board itself moves, or a person carries it away | eye: `eye_self_motion` | `board-was-moved` |

## Why the model is on the board

Almost all rows have the same three reasons:

- **Power.** The board makes the decision. Thus the radio, the SD card or the larger computer starts only when there
  is something to tell (72, 73).
- **Privacy.** A photo or one second of sound becomes one line, for example `person_present=true`. By default, that
  line is all that leaves the board, through USB or as an alert. This makes a camera in a bathroom corridor or a
  microphone in a meeting room acceptable (18, 26, 53). A photo leaves the board only for a rule where you set photos
  on. Photos are off in all builds except `trail-camera`. Sound never leaves the board.
- **No network is necessary.** The board operates in a garden, on a bike and in a pocket. Wi-Fi is necessary only for
  alerts and for the setup page.

## Rules for base answers

A build is a base model and a set of rules. The rules of each build are in `recipes/*.yaml`. You can change them on
the web page of the board, and you do not write the firmware again. In a project, the same rules go below `actions:`
in `decidio.yaml`:

```yaml
actions:
  # 1: lights while someone is in the room (held for 5 minutes after the last sighting)
  - {name: occupied, when: person_present = true, above: 0.8, for_ms: 300, gpio: 4, hold_ms: 300000, notify: false}
  # 2: the TV is on and nobody has been watching for 10 minutes
  - name: tv_on_alone
    when: [subject = electronics @ 0.5, person_present != true]     # up to 3 conditions; all must hold
    for_ms: 600000
    notify: {text: "The TV is on and nobody is watching", cooldown_s: 1800}
  # 15: the washing machine stopped (alert when the rule ends, in the daytime only)
  - {name: washer_done, when: home_sound = appliance, above: 0.7, for_ms: 120000, hold_ms: 60000, on: end,
     hours: "07:00-22:00", notify: {text: "The washer finished"}}
  # 7: count birds at the feeder, one message a day
  - {name: bird, when: animal_type = bird, above: 0.7, hold_ms: 5000, count: daily, notify: false}
  # 81: movement in the room, from the Wi-Fi signal
  - {name: movement, when: radio_motion > 0.055, for_ms: 1000, hold_ms: 30000}
```

| Field | Meaning |
|---|---|
| `when` | One to three conditions. All conditions must be true. A condition is `question = option` or `question != option`, each with its own threshold if you want one (`@ 0.5`). It can also be `radio_motion > x` or `radio_motion < x`, or an input with `>` or `<`, for example `eye_moving > 0.5` |
| `above` | The probability that a condition must have (default 0.8). `!=` is true when the probability of the option is below 1 - above |
| `for_ms`, `hold_ms` | The time that the condition must be true before the rule comes on. The time that the rule stays on after the condition was last true |
| `on` | The change that sends the alert: `start` (default), `end` or `both` |
| `hours` | The rule operates only in these hours, for example `"22:00-07:00"`. The board must have Wi-Fi for the clock. The rule stays off until the clock is set |
| `gpio`, `level` | The pin that the rule sets while it is on (`-1`: no pin). `level: 1` sets the pin high while the rule is on |
| `notify` | `false`, or `{text, photo, cooldown_s}`: the text of the alert, a camera picture with the alert (Telegram only), and the shortest time between two alerts |
| `count: daily` | Counts how many times and how long the rule was on. The board sends the result as one message each day |

You can set two more things for a rule only on the page of the board, because they use names from one home:

| On the page | Meaning |
|---|---|
| Switch a device | A Smart Life or Tuya device from the Devices tab of the board. The rule can set the device on while the rule is on, or off while the rule is on. It can also switch the device one time when the rule starts |
| A question that starts with `board` | A rule of a different board in the same group is on (or off). The condition is false while that board has not sent its state for 9 seconds |

Without Wi-Fi, a rule continues to set its pin and to write `{"event": ...}` to USB.

---

decidio is a product of [IO Services](https://ioservices.io/). Built in the USA.
