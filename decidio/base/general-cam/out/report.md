# general-cam - evaluation

Model: 320,055 parameters, 7.76M multiply-adds per decision. Trained on 84,449 samples, evaluated on 14,981 held-out samples.

| question | n | accuracy | majority baseline | int8 accuracy | notes |
|---|---|---|---|---|---|
| person_present | 11588 | 0.848 | 0.523 | 0.843 | AUC 0.924, best F1 0.839 at p>0.45 |
| animal_present | 12212 | 0.883 | 0.816 | 0.859 | AUC 0.930, best F1 0.747 at p>0.70 |
| vehicle_present | 12071 | 0.878 | 0.806 | 0.898 | AUC 0.942, best F1 0.758 at p>0.65 |
| subject | 11885 | 0.641 | 0.323 | 0.633 |  |
| animal_type | 12212 | 0.862 | 0.816 | 0.842 |  |
| vehicle_type | 12071 | 0.846 | 0.806 | 0.861 |  |
| indoors | 9185 | 0.962 | 0.777 | 0.938 | AUC 0.993, best F1 0.929 at p>0.75 |
| place | 10509 | 0.670 | 0.498 | 0.671 |  |
| weather | 3700 | 0.802 | 0.345 | 0.796 |  |
| thing | 11885 | 0.529 | 0.323 |  | auxiliary: trained, not flashed |

## person_present

| option | n | recall | precision |
|---|---|---|---|
| false | 6056 | 0.868 | 0.845 |
| true | 5532 | 0.826 | 0.851 |

Confusion (rows = truth, columns = prediction): false, true

```
  5256     800
   964    4568
```

## animal_present

| option | n | recall | precision |
|---|---|---|---|
| false | 9964 | 0.903 | 0.952 |
| true | 2248 | 0.798 | 0.649 |

Confusion (rows = truth, columns = prediction): false, true

```
  8996     968
   455    1793
```

## vehicle_present

| option | n | recall | precision |
|---|---|---|---|
| false | 9732 | 0.886 | 0.960 |
| true | 2339 | 0.845 | 0.640 |

Confusion (rows = truth, columns = prediction): false, true

```
  8622    1110
   362    1977
```

## subject

| option | n | recall | precision |
|---|---|---|---|
| none | 213 | 0.338 | 0.377 |
| person | 3837 | 0.680 | 0.833 |
| animal | 1681 | 0.773 | 0.743 |
| vehicle | 1781 | 0.777 | 0.752 |
| food | 536 | 0.664 | 0.472 |
| tableware | 198 | 0.121 | 0.222 |
| furniture | 1566 | 0.538 | 0.581 |
| electronics | 320 | 0.606 | 0.474 |
| appliance | 395 | 0.694 | 0.423 |
| sports | 278 | 0.446 | 0.314 |
| bag | 243 | 0.230 | 0.283 |
| street | 456 | 0.491 | 0.376 |
| household | 381 | 0.423 | 0.385 |

Confusion (rows = truth, columns = prediction): none, person, animal, vehicle, food, tableware, furniture, electronics, appliance, sports, bag, street, household

```
    72       3       9      16       8       1       5       4      15      10       1      42      27
    16    2608     192     222      37       1     233      61      68     185      49     116      49
    23      99    1300      50      24       0      51      25      15      27      11      32      24
    20      91      63    1384       8       0      26       7      21      22      36      92      11
     0      10       6       5     356      30      88       3       9       1       2       5      21
     2       8       8       6      60      24      39       4      26       1       2       5      13
     8      89      63      34     185      49     842      52     139       2      20      20      63
     0      16      13       8       8       0      26     194      34       0       6       7       8
     1      14       7       2      15       1      53      18     274       0       0       5       5
     7      66      14      23       6       0       4       3       1     124       3      19       8
     7      43      15      35      11       1      18      10      16       6      56      10      15
    25      42      46      45       8       0      16      11       8      12       6     224      13
    10      40      14      10      28       1      49      17      22       5       6      18     161
```

## animal_type

| option | n | recall | precision |
|---|---|---|---|
| none | 9964 | 0.931 | 0.945 |
| cat | 381 | 0.554 | 0.410 |
| dog | 368 | 0.198 | 0.304 |
| bird | 191 | 0.382 | 0.349 |
| horse | 235 | 0.451 | 0.367 |
| farm | 320 | 0.491 | 0.545 |
| wild | 753 | 0.831 | 0.735 |

Confusion (rows = truth, columns = prediction): none, cat, dog, bird, horse, farm, wild

```
  9278     198      87     111     122      44     124
   110     211      41       5       2       3       9
   170      81      73       5      13      11      15
    87       7       5      73       1       4      14
    64       5       9       0     106      32      19
    63       4      16       4      31     157      45
    47       9       9      11      14      37     626
```

## vehicle_type

| option | n | recall | precision |
|---|---|---|---|
| none | 9732 | 0.923 | 0.949 |
| car | 522 | 0.464 | 0.313 |
| bus | 285 | 0.589 | 0.562 |
| truck | 306 | 0.294 | 0.466 |
| motorbike | 247 | 0.656 | 0.533 |
| bicycle | 165 | 0.158 | 0.239 |
| train | 308 | 0.727 | 0.659 |
| boat | 261 | 0.525 | 0.495 |
| airplane | 245 | 0.759 | 0.587 |

Confusion (rows = truth, columns = prediction): none, car, bus, truck, motorbike, bicycle, train, boat, airplane

```
  8978     329      38      21      65      38      68     103      92
   154     242      22      37      29      19       4       5      10
    25      36     168      24       6       8      12       3       3
    56      78      36      90       9       5      15      10       7
    45      18       1       3     162      12       1       4       1
    71      29       5       0      24      26       6       3       1
    32      14      19       9       0       1     224       6       3
    63      19       7       4       9       0       8     137      14
    35       8       3       5       0       0       2       6     186
```

## indoors

| option | n | recall | precision |
|---|---|---|---|
| false | 7137 | 0.960 | 0.990 |
| true | 2048 | 0.966 | 0.875 |

Confusion (rows = truth, columns = prediction): false, true

```
  6854     283
    69    1979
```

## place

| option | n | recall | precision |
|---|---|---|---|
| other | 5238 | 0.595 | 0.879 |
| kitchen | 318 | 0.676 | 0.459 |
| bathroom | 426 | 0.817 | 0.609 |
| bedroom | 329 | 0.556 | 0.469 |
| living_room | 295 | 0.536 | 0.370 |
| eating | 62 | 0.081 | 0.104 |
| office | 217 | 0.507 | 0.489 |
| street | 1272 | 0.827 | 0.560 |
| green | 1126 | 0.855 | 0.596 |
| nature | 578 | 0.696 | 0.702 |
| water | 648 | 0.748 | 0.635 |

Confusion (rows = truth, columns = prediction): other, kitchen, bathroom, bedroom, living_room, eating, office, street, green, nature, water

```
  3119     161     140     124     156      39      72     663     500     107     157
    26     215      30       8      25       3       7       4       0       0       0
    32      14     348      16       8       0       3       4       0       0       1
    40      12      32     183      36       0      16       4       3       1       2
    41      30      10      40     158       1      10       3       0       1       1
    25       8       0       1      15       5       0       7       1       0       0
    42      22       5       8      27       0     110       2       0       0       1
    95       5       3       1       2       0       5    1052      53      24      32
    50       0       0       1       0       0       2      67     963       7      36
    41       0       3       3       0       0       0      30      50     402      49
    39       1       0       5       0       0       0      41      46      31     485
```

## weather

| option | n | recall | precision |
|---|---|---|---|
| none | 1278 | 0.971 | 0.976 |
| clear | 808 | 0.791 | 0.814 |
| cloudy | 920 | 0.585 | 0.751 |
| rain | 272 | 0.717 | 0.492 |
| snow | 227 | 0.819 | 0.604 |
| fog | 195 | 0.867 | 0.758 |

Confusion (rows = truth, columns = prediction): none, clear, cloudy, rain, snow, fog

```
  1241       2      13      13       5       4
    10     639     100      32      24       3
    19     126     538     136      71      30
     1      11      44     195      13       8
     1       6      12      13     186       9
     0       1       9       7       9     169
```
