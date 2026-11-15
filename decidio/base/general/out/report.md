# general - evaluation

Model: 392,347 parameters, 8.90M multiply-adds per decision. Trained on 95,741 samples, evaluated on 16,943 held-out samples.

| question | n | accuracy | majority baseline | int8 accuracy | notes |
|---|---|---|---|---|---|
| person_present | 13137 | 0.846 | 0.522 | 0.846 | AUC 0.924, best F1 0.839 at p>0.40 |
| animal_present | 13836 | 0.884 | 0.815 | 0.891 | AUC 0.927, best F1 0.753 at p>0.80 |
| vehicle_present | 13681 | 0.877 | 0.807 | 0.882 | AUC 0.942, best F1 0.759 at p>0.75 |
| subject | 13466 | 0.636 | 0.325 | 0.646 |  |
| animal_type | 13836 | 0.861 | 0.815 | 0.868 |  |
| vehicle_type | 13681 | 0.843 | 0.807 | 0.844 |  |
| indoors | 10398 | 0.957 | 0.776 | 0.961 | AUC 0.993, best F1 0.925 at p>0.80 |
| place | 11889 | 0.678 | 0.499 | 0.688 |  |
| weather | 4155 | 0.807 | 0.347 | 0.796 |  |
| sound_group | 16548 | 0.863 | 0.634 | 0.864 |  |
| alert_sound | 16548 | 0.973 | 0.964 | 0.974 | AUC 0.979, best F1 0.675 at p>0.60 |
| speech_present | 16943 | 0.986 | 0.620 | 0.988 | AUC 0.999, best F1 0.989 at p>0.60 |
| home_sound | 16548 | 0.912 | 0.913 | 0.916 |  |
| weather_sound | 16548 | 0.983 | 0.987 | 0.986 |  |
| thing | 13466 | 0.526 | 0.325 |  | auxiliary: trained, not flashed |
| sound | 16548 | 0.823 | 0.634 |  | auxiliary: trained, not flashed |
| word | 12599 | 0.802 | 0.480 |  | auxiliary: trained, not flashed |

## person_present

| option | n | recall | precision |
|---|---|---|---|
| false | 6858 | 0.877 | 0.836 |
| true | 6279 | 0.812 | 0.858 |

Confusion (rows = truth, columns = prediction): false, true

```
  6016     842
  1183    5096
```

## animal_present

| option | n | recall | precision |
|---|---|---|---|
| false | 11283 | 0.901 | 0.954 |
| true | 2553 | 0.807 | 0.649 |

Confusion (rows = truth, columns = prediction): false, true

```
 10166    1117
   492    2061
```

## vehicle_present

| option | n | recall | precision |
|---|---|---|---|
| false | 11039 | 0.883 | 0.961 |
| true | 2642 | 0.852 | 0.635 |

Confusion (rows = truth, columns = prediction): false, true

```
  9747    1292
   391    2251
```

## subject

| option | n | recall | precision |
|---|---|---|---|
| none | 250 | 0.352 | 0.444 |
| person | 4380 | 0.648 | 0.852 |
| animal | 1897 | 0.776 | 0.735 |
| vehicle | 2008 | 0.790 | 0.731 |
| food | 609 | 0.670 | 0.477 |
| tableware | 226 | 0.093 | 0.214 |
| furniture | 1763 | 0.538 | 0.566 |
| electronics | 361 | 0.590 | 0.464 |
| appliance | 448 | 0.723 | 0.441 |
| sports | 313 | 0.412 | 0.289 |
| bag | 276 | 0.304 | 0.271 |
| street | 511 | 0.495 | 0.374 |
| household | 424 | 0.453 | 0.380 |

Confusion (rows = truth, columns = prediction): none, person, animal, vehicle, food, tableware, furniture, electronics, appliance, sports, bag, street, household

```
    88       4       7      24      11       1      10       6      17      12       0      43      27
    15    2840     235     281      48       1     287      60      90     221      86     149      67
    21      82    1472      71      25       0      64      24      12      22      22      37      45
    22      81      72    1586       9       1      28      10      17      34      38      87      23
     0      12       7       7     408      27     103       2      13       2       1       5      22
     1       6       7       5      74      21      52      11      24       1       2       5      17
     6      96      87      32     199      38     949      76     156       1      36      18      69
     0      15       9      14      11       3      37     213      31       4       8       8       8
     0      13      10       1      20       0      53      16     324       0       1       3       7
     3      73      14      35       5       1       3       5       5     129       9      24       7
     3      44      10      33      11       0      25      13      16       7      84      21       9
    30      38      50      64       4       0      24       8      10      11       7     253      12
     9      28      23      18      30       5      42      15      20       2      16      24     192
```

## animal_type

| option | n | recall | precision |
|---|---|---|---|
| none | 11283 | 0.931 | 0.945 |
| cat | 439 | 0.569 | 0.399 |
| dog | 422 | 0.180 | 0.267 |
| bird | 222 | 0.338 | 0.342 |
| horse | 266 | 0.436 | 0.430 |
| farm | 348 | 0.474 | 0.498 |
| wild | 856 | 0.848 | 0.736 |

Confusion (rows = truth, columns = prediction): none, cat, dog, bird, horse, farm, wild

```
 10508     240     134     120      98      54     129
   128     250      39       5       3       3      11
   189     109      76       4       8      12      24
   107       5       6      75       0       7      22
    72       9       7       0     116      44      18
    72       4      18       4      29     165      56
    42      10       5      11      16      46     726
```

## vehicle_type

| option | n | recall | precision |
|---|---|---|---|
| none | 11039 | 0.918 | 0.952 |
| car | 582 | 0.479 | 0.288 |
| bus | 333 | 0.589 | 0.598 |
| truck | 340 | 0.294 | 0.429 |
| motorbike | 279 | 0.677 | 0.604 |
| bicycle | 186 | 0.151 | 0.269 |
| train | 349 | 0.745 | 0.613 |
| boat | 298 | 0.480 | 0.461 |
| airplane | 275 | 0.756 | 0.579 |

Confusion (rows = truth, columns = prediction): none, car, bus, truck, motorbike, bicycle, train, boat, airplane

```
 10129     421      42      23      60      38      94     126     106
   162     279      22      48      22      18      10      11      10
    26      57     196      28       4       5      12       3       2
    59      94      40     100       4       6      19      11       7
    45      26       0       2     189       7       4       4       2
    76      34       4       6      26      28       7       4       1
    35      19      17      10       0       0     260       5       3
    82      20       6       8       8       0      11     143      20
    26      20       1       8       0       2       7       3     208
```

## indoors

| option | n | recall | precision |
|---|---|---|---|
| false | 8065 | 0.954 | 0.991 |
| true | 2333 | 0.970 | 0.858 |

Confusion (rows = truth, columns = prediction): false, true

```
  7690     375
    71    2262
```

## place

| option | n | recall | precision |
|---|---|---|---|
| other | 5930 | 0.601 | 0.887 |
| kitchen | 360 | 0.708 | 0.447 |
| bathroom | 479 | 0.818 | 0.623 |
| bedroom | 377 | 0.568 | 0.471 |
| living_room | 330 | 0.555 | 0.368 |
| eating | 65 | 0.138 | 0.115 |
| office | 254 | 0.531 | 0.508 |
| street | 1438 | 0.838 | 0.563 |
| green | 1269 | 0.842 | 0.605 |
| nature | 659 | 0.709 | 0.744 |
| water | 728 | 0.776 | 0.673 |

Confusion (rows = truth, columns = prediction): other, kitchen, bathroom, bedroom, living_room, eating, office, street, green, nature, water

```
  3564     198     152     147     187      61      87     744     528     101     161
    30     255      25      10      21       6       8       5       0       0       0
    33      16     392      25      10       0       1       1       0       0       1
    45      16      32     214      45       0      13       8       3       0       1
    42      31      16      37     183       0      18       1       0       1       1
    23      11       0       1      13       9       1       5       1       0       1
    39      31       6      10      30       1     135       2       0       0       0
    97      10       4       1       7       1       2    1205      61      22      28
    70       0       0       1       0       0       1      88    1069      13      27
    37       1       1       4       1       0       0      37      57     467      54
    40       1       1       4       0       0       0      45      48      24     565
```

## weather

| option | n | recall | precision |
|---|---|---|---|
| none | 1440 | 0.977 | 0.973 |
| clear | 919 | 0.752 | 0.829 |
| cloudy | 1013 | 0.616 | 0.746 |
| rain | 305 | 0.770 | 0.527 |
| snow | 262 | 0.805 | 0.603 |
| fog | 216 | 0.861 | 0.769 |

Confusion (rows = truth, columns = prediction): none, clear, cloudy, rain, snow, fog

```
  1407       9      10       8       2       4
    16     691     139      38      33       2
    20     117     624     141      82      29
     3       9      33     235      16       9
     0       6      18      15     211      12
     0       2      13       9       6     186
```

## sound_group

| option | n | recall | precision |
|---|---|---|---|
| quiet | 641 | 1.000 | 0.995 |
| animal | 756 | 0.630 | 0.455 |
| nature | 1184 | 0.818 | 0.638 |
| human | 1292 | 0.402 | 0.636 |
| speech | 10497 | 0.983 | 0.992 |
| household | 1069 | 0.674 | 0.643 |
| outside | 1109 | 0.573 | 0.638 |

Confusion (rows = truth, columns = prediction): quiet, animal, nature, human, speech, household, outside

```
   641       0       0       0       0       0       0
     0     476      19      78       0      60     123
     0      51     968      52      19      34      60
     3     212     252     520      55     178      72
     0     106      11      49   10320      11       0
     0      40     142      49      11     721     106
     0     161     125      69       0     118     636
```

## alert_sound

| option | n | recall | precision |
|---|---|---|---|
| false | 15959 | 0.985 | 0.987 |
| true | 589 | 0.640 | 0.609 |

Confusion (rows = truth, columns = prediction): false, true

```
 15717     242
   212     377
```

## speech_present

| option | n | recall | precision |
|---|---|---|---|
| false | 6446 | 0.977 | 0.987 |
| true | 10497 | 0.992 | 0.986 |

Confusion (rows = truth, columns = prediction): false, true

```
  6300     146
    83   10414
```

## home_sound

| option | n | recall | precision |
|---|---|---|---|
| none | 15105 | 0.933 | 0.975 |
| dog | 44 | 0.841 | 0.333 |
| baby | 163 | 0.620 | 0.701 |
| cough | 77 | 0.766 | 0.407 |
| knock | 91 | 0.813 | 0.860 |
| glass | 41 | 0.902 | 0.425 |
| alarm | 44 | 0.545 | 1.000 |
| water | 464 | 0.769 | 0.532 |
| appliance | 210 | 0.786 | 0.387 |
| footsteps | 52 | 0.577 | 0.236 |
| snoring | 79 | 0.456 | 0.226 |
| clap | 178 | 0.382 | 0.596 |

Confusion (rows = truth, columns = prediction): none, dog, baby, cough, knock, glass, alarm, water, appliance, footsteps, snoring, clap

```
 14099      74      43      63      12      40       0     298     225      97     116      38
     7      37       0       0       0       0       0       0       0       0       0       0
    51       0     101      11       0       0       0       0       0       0       0       0
    18       0       0      59       0       0       0       0       0       0       0       0
    17       0       0       0      74       0       0       0       0       0       0       0
     0       0       0       0       0      37       0       4       0       0       0       0
    20       0       0       0       0       0      24       0       0       0       0       0
    68       0       0      12       0       0       0     357      19       0       0       8
    38       0       0       0       0       0       0       0     165       0       7       0
    22       0       0       0       0       0       0       0       0      30       0       0
    26       0       0       0       0       0       0       0      17       0      36       0
    88       0       0       0       0      10       0      12       0       0       0      68
```

## weather_sound

| option | n | recall | precision |
|---|---|---|---|
| none | 16327 | 0.985 | 0.998 |
| rain | 139 | 1.000 | 0.454 |
| wind | 26 | 0.000 | 0.000 |
| thunder | 56 | 1.000 | 0.737 |

Confusion (rows = truth, columns = prediction): none, rain, wind, thunder

```
 16077     167      63      20
     0     139       0       0
    26       0       0       0
     0       0       0      56
```
