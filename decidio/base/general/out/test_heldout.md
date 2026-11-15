# general - test on held-out data (COCO val2017, weather test photos, ESC-50 fold 1, Speech Commands test list, LibriSpeech test-clean) (int8)

| question | n | accuracy | majority baseline | int8 accuracy | notes |
|---|---|---|---|---|---|
| person_present | 12063 | 0.844 | 0.526 | 0.844 | AUC 0.923, best F1 0.836 at p>0.50 |
| animal_present | 12753 | 0.890 | 0.821 | 0.890 | AUC 0.937, best F1 0.769 at p>0.80 |
| vehicle_present | 12558 | 0.861 | 0.812 | 0.861 | AUC 0.937, best F1 0.749 at p>0.80 |
| subject | 12330 | 0.644 | 0.316 | 0.644 |  |
| animal_type | 12753 | 0.869 | 0.821 | 0.869 |  |
| vehicle_type | 12558 | 0.833 | 0.812 | 0.833 |  |
| indoors | 9631 | 0.959 | 0.765 | 0.959 | AUC 0.988, best F1 0.922 at p>0.70 |
| place | 10966 | 0.676 | 0.483 | 0.676 |  |
| weather | 3679 | 0.801 | 0.360 | 0.801 |  |
| sound_group | 15432 | 0.887 | 0.693 | 0.887 |  |
| alert_sound | 15432 | 0.971 | 0.969 | 0.971 | AUC 0.969, best F1 0.640 at p>0.75 |
| speech_present | 15432 | 0.986 | 0.693 | 0.986 | AUC 0.999, best F1 0.990 at p>0.65 |
| home_sound | 15432 | 0.937 | 0.922 | 0.937 |  |
| weather_sound | 15432 | 0.981 | 0.981 | 0.981 |  |

## person_present

| option | n | recall | precision |
|---|---|---|---|
| false | 6349 | 0.861 | 0.846 |
| true | 5714 | 0.826 | 0.842 |

Confusion (rows = truth, columns = prediction): false, true

```
  5466     883
   993    4721
```

## animal_present

| option | n | recall | precision |
|---|---|---|---|
| false | 10471 | 0.903 | 0.960 |
| true | 2282 | 0.830 | 0.650 |

Confusion (rows = truth, columns = prediction): false, true

```
  9451    1020
   389    1893
```

## vehicle_present

| option | n | recall | precision |
|---|---|---|---|
| false | 10194 | 0.862 | 0.963 |
| true | 2364 | 0.856 | 0.590 |

Confusion (rows = truth, columns = prediction): false, true

```
  8789    1405
   341    2023
```

## subject

| option | n | recall | precision |
|---|---|---|---|
| none | 231 | 0.411 | 0.492 |
| person | 3901 | 0.682 | 0.822 |
| animal | 1723 | 0.817 | 0.709 |
| vehicle | 1831 | 0.790 | 0.702 |
| food | 554 | 0.668 | 0.458 |
| tableware | 247 | 0.154 | 0.355 |
| furniture | 1617 | 0.503 | 0.615 |
| electronics | 385 | 0.639 | 0.548 |
| appliance | 433 | 0.681 | 0.474 |
| sports | 311 | 0.428 | 0.324 |
| bag | 215 | 0.237 | 0.259 |
| street | 493 | 0.481 | 0.412 |
| household | 389 | 0.375 | 0.403 |

Confusion (rows = truth, columns = prediction): none, person, animal, vehicle, food, tableware, furniture, electronics, appliance, sports, bag, street, household

```
    95       3      15      18       7       0      11       2      17       6       0      40      17
    18    2662     240     264      54       0     181      37      46     181      62     104      52
     6      83    1407      66      26       0      29      31       8      18      11      26      12
    28     107      65    1446       7       0      15      20      17      23      20      77       6
     2       8      17       9     370      28      72       8       9       1       0      10      20
     0       8      12       7      87      38      50      10      25       2       0       1       7
     7     125      93      39     185      33     813      52     145      11      24      29      61
     1      19      16      15       0       0      34     246      30       2       5       7      10
     0      16      14       5      13       3      52      18     295       0       3       4      10
     6      93      19      30       4       0       1       4       1     133       6      11       3
     2      43      17      43       4       0      11       6       8       7      51       8      15
    16      44      36      92       9       0      19       1       6      21       9     237       3
    12      29      34      27      41       5      33      14      15       6       6      21     146
```

## animal_type

| option | n | recall | precision |
|---|---|---|---|
| none | 10471 | 0.930 | 0.953 |
| cat | 418 | 0.596 | 0.472 |
| dog | 335 | 0.272 | 0.318 |
| bird | 182 | 0.390 | 0.390 |
| horse | 270 | 0.526 | 0.445 |
| farm | 305 | 0.528 | 0.486 |
| wild | 772 | 0.813 | 0.708 |

Confusion (rows = truth, columns = prediction): none, cat, dog, bird, horse, farm, wild

```
  9742     205     128      75     109      81     131
   112     249      32       9       6       5       5
   131      53      91       8      11      15      26
    77       2      11      71       2       4      15
    65       1       4       0     142      16      42
    49       7      12       3      33     161      40
    44      11       8      16      16      49     628
```

## vehicle_type

| option | n | recall | precision |
|---|---|---|---|
| none | 10194 | 0.907 | 0.950 |
| car | 496 | 0.504 | 0.259 |
| bus | 361 | 0.515 | 0.585 |
| truck | 286 | 0.241 | 0.373 |
| motorbike | 269 | 0.643 | 0.506 |
| bicycle | 170 | 0.206 | 0.368 |
| train | 325 | 0.708 | 0.635 |
| boat | 240 | 0.525 | 0.472 |
| airplane | 217 | 0.710 | 0.520 |

Confusion (rows = truth, columns = prediction): none, car, bus, truck, motorbike, bicycle, train, boat, airplane

```
  9244     469      42      27      87      46      68     107     104
   155     250      26      15      23       7       8       7       5
    28      71     186      42       2       4      22       5       1
    68      88      29      69       4       0      14       8       6
    55      24       3       8     173       2       2       2       0
    52      25       4       0      46      35       6       0       2
    36      10      22      19       0       1     230       6       1
    59      15       3       0       4       0      10     126      23
    29      15       3       5       3       0       2       6     154
```

## indoors

| option | n | recall | precision |
|---|---|---|---|
| false | 7366 | 0.962 | 0.984 |
| true | 2265 | 0.948 | 0.885 |

Confusion (rows = truth, columns = prediction): false, true

```
  7088     278
   118    2147
```

## place

| option | n | recall | precision |
|---|---|---|---|
| other | 5295 | 0.623 | 0.859 |
| kitchen | 335 | 0.666 | 0.399 |
| bathroom | 491 | 0.697 | 0.702 |
| bedroom | 321 | 0.455 | 0.465 |
| living_room | 306 | 0.572 | 0.449 |
| eating | 74 | 0.176 | 0.141 |
| office | 245 | 0.596 | 0.551 |
| street | 1405 | 0.827 | 0.548 |
| green | 1179 | 0.820 | 0.630 |
| nature | 568 | 0.660 | 0.700 |
| water | 747 | 0.752 | 0.682 |

Confusion (rows = truth, columns = prediction): other, kitchen, bathroom, bedroom, living_room, eating, office, street, green, nature, water

```
  3299     183      88     101     110      75      91     714     400      89     145
    33     223      20       9      34       2       4      10       0       0       0
    70      36     342      15       9       0       9       8       2       0       0
    83      19      21     146      34       0       5       5       0       5       3
    37      34       9      32     175       1       9       8       1       0       0
    21      19       1       2       9      13       0       8       0       0       1
    45      27       2       6      16       1     146       0       0       0       2
   107      15       3       1       2       0       1    1162      63      21      30
    70       3       1       0       0       0       0      88     967      13      37
    34       0       0       0       0       0       0      52      63     375      44
    42       0       0       2       1       0       0      67      40      33     562
```

## weather

| option | n | recall | precision |
|---|---|---|---|
| none | 1325 | 0.956 | 0.980 |
| clear | 823 | 0.729 | 0.863 |
| cloudy | 865 | 0.616 | 0.746 |
| rain | 246 | 0.809 | 0.439 |
| snow | 256 | 0.812 | 0.634 |
| fog | 164 | 0.848 | 0.709 |

Confusion (rows = truth, columns = prediction): none, clear, cloudy, rain, snow, fog

```
  1267      14       8      19      10       7
     9     600     129      52      30       3
    16      65     533     154      64      33
     0       3      20     199      11      13
     1      12      11      23     208       1
     0       1      13       6       5     139
```

## sound_group

| option | n | recall | precision |
|---|---|---|---|
| quiet | 322 | 1.000 | 0.877 |
| animal | 915 | 0.685 | 0.620 |
| nature | 926 | 0.751 | 0.683 |
| human | 835 | 0.583 | 0.627 |
| speech | 10702 | 0.987 | 0.991 |
| household | 795 | 0.546 | 0.673 |
| outside | 937 | 0.604 | 0.590 |

Confusion (rows = truth, columns = prediction): quiet, animal, nature, human, speech, household, outside

```
   322       0       0       0       0       0       0
    14     627      33      75      18      21     127
    12      28     695      39       8      90      54
    12      48      78     487      43      68      99
     0      79      14      43   10558       8       0
     4      74      61      87      22     434     113
     3     156     137      46       5      24     566
```

## alert_sound

| option | n | recall | precision |
|---|---|---|---|
| false | 14956 | 0.979 | 0.990 |
| true | 476 | 0.693 | 0.516 |

Confusion (rows = truth, columns = prediction): false, true

```
 14647     309
   146     330
```

## speech_present

| option | n | recall | precision |
|---|---|---|---|
| false | 4730 | 0.968 | 0.986 |
| true | 10702 | 0.994 | 0.986 |

Confusion (rows = truth, columns = prediction): false, true

```
  4577     153
    67   10635
```

## home_sound

| option | n | recall | precision |
|---|---|---|---|
| none | 14225 | 0.966 | 0.973 |
| dog | 67 | 0.821 | 0.591 |
| baby | 105 | 0.810 | 0.563 |
| cough | 111 | 0.378 | 0.393 |
| knock | 59 | 0.881 | 0.571 |
| glass | 49 | 0.694 | 0.395 |
| alarm | 85 | 0.659 | 0.918 |
| water | 272 | 0.717 | 0.670 |
| appliance | 179 | 0.408 | 0.525 |
| footsteps | 89 | 0.124 | 0.108 |
| snoring | 98 | 0.571 | 0.505 |
| clap | 93 | 0.645 | 0.800 |

Confusion (rows = truth, columns = prediction): none, dog, baby, cough, knock, glass, alarm, water, appliance, footsteps, snoring, clap

```
 13743      36      62      52      39      30       2      69      64      64      55       9
     6      55       0       6       0       0       0       0       0       0       0       0
     4       2      85       4       0       0       3       0       0       7       0       0
    40       0       4      42       0       6       0      16       0       0       0       3
     4       0       0       3      52       0       0       0       0       0       0       0
     9       0       0       0       0      34       0       2       0       4       0       0
    29       0       0       0       0       0      56       0       0       0       0       0
    70       0       0       0       0       2       0     195       0       5       0       0
   104       0       0       0       0       0       0       2      73       0       0       0
    63       0       0       0       0       7       0       5       0      11       0       3
    42       0       0       0       0       0       0       0       0       0      56       0
    11       0       0       0       0       7       0       2       2      11       0      60
```

## weather_sound

| option | n | recall | precision |
|---|---|---|---|
| none | 15141 | 0.988 | 0.996 |
| rain | 88 | 0.852 | 0.436 |
| wind | 89 | 0.247 | 0.289 |
| thunder | 114 | 0.640 | 0.474 |

Confusion (rows = truth, columns = prediction): none, rain, wind, thunder

```
 14963      97      37      44
    13      75       0       0
    30       0      22      37
    24       0      17      73
```
