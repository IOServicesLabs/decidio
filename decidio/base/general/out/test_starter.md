# general - test on the starter set (int8)

| question | n | accuracy | majority baseline | int8 accuracy | notes |
|---|---|---|---|---|---|
| person_present | 2582 | 0.857 | 0.526 | 0.857 | AUC 0.927, best F1 0.851 at p>0.50 |
| animal_present | 2725 | 0.881 | 0.830 | 0.881 | AUC 0.935, best F1 0.744 at p>0.75 |
| vehicle_present | 2682 | 0.862 | 0.837 | 0.862 | AUC 0.943, best F1 0.734 at p>0.80 |
| subject | 2632 | 0.619 | 0.322 | 0.619 |  |
| animal_type | 2725 | 0.854 | 0.830 | 0.854 |  |
| vehicle_type | 2682 | 0.836 | 0.837 | 0.836 |  |
| indoors | 0 | | | | no labeled validation samples |
| place | 0 | | | | no labeled validation samples |
| weather | 0 | | | | no labeled validation samples |
| sound_group | 2783 | 0.673 | 0.190 | 0.673 |  |
| alert_sound | 2783 | 0.893 | 0.895 | 0.893 | AUC 0.906, best F1 0.616 at p>0.60 |
| speech_present | 2783 | 0.963 | 1.000 | 0.963 | AUC nan, best F1 0.000 at p>0.95 |
| home_sound | 2783 | 0.803 | 0.731 | 0.803 |  |
| weather_sound | 2783 | 0.943 | 0.942 | 0.943 |  |

## person_present

| option | n | recall | precision |
|---|---|---|---|
| false | 1358 | 0.867 | 0.862 |
| true | 1224 | 0.846 | 0.852 |

Confusion (rows = truth, columns = prediction): false, true

```
  1178     180
   188    1036
```

## animal_present

| option | n | recall | precision |
|---|---|---|---|
| false | 2263 | 0.893 | 0.962 |
| true | 462 | 0.827 | 0.611 |

Confusion (rows = truth, columns = prediction): false, true

```
  2020     243
    80     382
```

## vehicle_present

| option | n | recall | precision |
|---|---|---|---|
| false | 2246 | 0.862 | 0.970 |
| true | 436 | 0.865 | 0.549 |

Confusion (rows = truth, columns = prediction): false, true

```
  1936     310
    59     377
```

## subject

| option | n | recall | precision |
|---|---|---|---|
| none | 63 | 0.492 | 0.608 |
| person | 847 | 0.668 | 0.814 |
| animal | 362 | 0.776 | 0.669 |
| vehicle | 320 | 0.791 | 0.637 |
| food | 149 | 0.577 | 0.506 |
| tableware | 55 | 0.109 | 0.188 |
| furniture | 314 | 0.471 | 0.576 |
| electronics | 70 | 0.614 | 0.434 |
| appliance | 106 | 0.679 | 0.541 |
| sports | 75 | 0.480 | 0.387 |
| bag | 47 | 0.149 | 0.163 |
| street | 144 | 0.472 | 0.433 |
| household | 80 | 0.412 | 0.388 |

Confusion (rows = truth, columns = prediction): none, person, animal, vehicle, food, tableware, furniture, electronics, appliance, sports, bag, street, household

```
    31       0       4       4       2       0       2       0       4       0       0      11       5
     1     566      62      50      17       0      43       8       5      44      15      23      13
     2      21     281      12       2       0       2       9      10       2       2      15       4
     4      20       7     253       0       0       2       8       3       1       5      16       1
     2      10       7       0      86      15      15       1       6       0       0       5       2
     0       2       8       2      20       6      14       0       1       0       0       0       2
     1      34      12      13      30      11     148      15      26       2       2       5      15
     1       4       7       3       0       0       6      43       2       0       3       0       1
     0       9       0       2       0       0      14       3      72       0       2       2       2
     1      10       8      10       0       0       0       1       0      36       2       5       2
     1       5       2      16       0       0       2       3       1       2       7       5       3
     6      10      14      26       6       0       4       3       0       5       0      68       2
     1       4       8       6       7       0       5       5       3       1       5       2      33
```

## animal_type

| option | n | recall | precision |
|---|---|---|---|
| none | 2263 | 0.923 | 0.952 |
| cat | 64 | 0.562 | 0.350 |
| dog | 64 | 0.250 | 0.246 |
| bird | 22 | 0.273 | 0.171 |
| horse | 60 | 0.383 | 0.261 |
| farm | 82 | 0.244 | 0.385 |
| wild | 170 | 0.806 | 0.733 |

Confusion (rows = truth, columns = prediction): none, cat, dog, bird, horse, farm, wild

```
  2089      46      35      17      46      13      17
    27      36       0       0       0       1       0
    22      15      16       5       2       4       0
    12       0       1       6       2       0       1
    18       2       2       0      23       4      11
    18       1       8       0      14      20      21
     9       3       3       7       1      10     137
```

## vehicle_type

| option | n | recall | precision |
|---|---|---|---|
| none | 2246 | 0.905 | 0.958 |
| car | 110 | 0.491 | 0.249 |
| bus | 55 | 0.509 | 0.483 |
| truck | 60 | 0.233 | 0.326 |
| motorbike | 38 | 0.658 | 0.357 |
| bicycle | 31 | 0.226 | 0.333 |
| train | 52 | 0.538 | 0.519 |
| boat | 50 | 0.460 | 0.500 |
| airplane | 40 | 0.750 | 0.588 |

Confusion (rows = truth, columns = prediction): none, car, bus, truck, motorbike, bicycle, train, boat, airplane

```
  2032     119       8      10      20       9      11      20      17
    28      54       8       5       9       4       2       0       0
     8       5      28       7       4       1       2       0       0
    18      14       7      14       3       0       4       0       0
     9       2       0       0      25       0       1       1       0
     6       8       0       0       9       7       1       0       0
     5       7       6       5       0       0      28       1       0
    12       6       0       0       0       0       5      23       4
     4       2       1       2       0       0       0       1      30
```

## sound_group

| option | n | recall | precision |
|---|---|---|---|
| quiet | 188 | 1.000 | 0.969 |
| animal | 529 | 0.715 | 0.685 |
| nature | 517 | 0.749 | 0.677 |
| human | 508 | 0.618 | 0.674 |
| speech | 0 | 0.000 | 0.000 |
| household | 520 | 0.558 | 0.755 |
| outside | 521 | 0.608 | 0.575 |

Confusion (rows = truth, columns = prediction): quiet, animal, nature, human, speech, household, outside

```
   188       0       0       0       0       0       0
     0     378      13      35      11       5      87
     3      13     387      23       4      56      31
     3      35      52     314      29      29      46
     0       0       0       0       0       0       0
     0      42      40      59      19     290      70
     0      84      80      35       1       4     317
```

## alert_sound

| option | n | recall | precision |
|---|---|---|---|
| false | 2491 | 0.917 | 0.962 |
| true | 292 | 0.688 | 0.493 |

Confusion (rows = truth, columns = prediction): false, true

```
  2284     207
    91     201
```

## speech_present

| option | n | recall | precision |
|---|---|---|---|
| false | 2783 | 0.963 | 1.000 |
| true | 0 | 0.000 | 0.000 |

Confusion (rows = truth, columns = prediction): false, true

```
  2679     104
     0       0
```

## home_sound

| option | n | recall | precision |
|---|---|---|---|
| none | 2034 | 0.872 | 0.887 |
| dog | 50 | 0.840 | 0.750 |
| baby | 52 | 0.942 | 0.510 |
| cough | 99 | 0.515 | 0.600 |
| knock | 46 | 0.848 | 0.750 |
| glass | 35 | 0.514 | 0.333 |
| alarm | 56 | 0.589 | 0.917 |
| water | 166 | 0.729 | 0.729 |
| appliance | 109 | 0.422 | 0.548 |
| footsteps | 49 | 0.061 | 0.060 |
| snoring | 49 | 0.694 | 0.540 |
| clap | 38 | 0.684 | 0.619 |

Confusion (rows = truth, columns = prediction): none, dog, baby, cough, knock, glass, alarm, water, appliance, footsteps, snoring, clap

```
  1773      14      40      28      13      21       3      32      37      36      29       8
     5      42       0       3       0       0       0       0       0       0       0       0
     0       0      49       0       0       0       0       0       0       3       0       0
    28       0       7      51       0       6       0       3       0       0       0       4
     4       0       0       3      39       0       0       0       0       0       0       0
     9       0       0       0       0      18       0       6       0       2       0       0
    23       0       0       0       0       0      33       0       0       0       0       0
    43       0       0       0       0       0       0     121       0       2       0       0
    63       0       0       0       0       0       0       0      46       0       0       0
    35       0       0       0       0       3       0       4       0       3       0       4
    15       0       0       0       0       0       0       0       0       0      34       0
     1       0       0       0       0       6       0       0       1       4       0      26
```

## weather_sound

| option | n | recall | precision |
|---|---|---|---|
| none | 2622 | 0.965 | 0.987 |
| rain | 61 | 0.869 | 0.530 |
| wind | 56 | 0.179 | 0.357 |
| thunder | 44 | 0.750 | 0.359 |

Confusion (rows = truth, columns = prediction): none, rain, wind, thunder

```
  2529      47      14      32
     8      53       0       0
    19       0      10      27
     7       0       4      33
```
