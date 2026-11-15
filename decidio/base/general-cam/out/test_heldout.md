# general-cam - test on held-out data (COCO val2017, weather test photos, ESC-50 fold 1, Speech Commands test list, LibriSpeech test-clean) (int8)

| question | n | accuracy | majority baseline | int8 accuracy | notes |
|---|---|---|---|---|---|
| person_present | 8995 | 0.844 | 0.525 | 0.844 | AUC 0.922, best F1 0.833 at p>0.40 |
| animal_present | 9518 | 0.860 | 0.820 | 0.860 | AUC 0.932, best F1 0.760 at p>0.85 |
| vehicle_present | 9377 | 0.895 | 0.813 | 0.895 | AUC 0.935, best F1 0.736 at p>0.65 |
| subject | 9207 | 0.629 | 0.317 | 0.629 |  |
| animal_type | 9518 | 0.845 | 0.820 | 0.845 |  |
| vehicle_type | 9377 | 0.865 | 0.813 | 0.865 |  |
| indoors | 7187 | 0.937 | 0.767 | 0.937 | AUC 0.986, best F1 0.906 at p>0.85 |
| place | 8164 | 0.670 | 0.483 | 0.670 |  |
| weather | 2768 | 0.796 | 0.357 | 0.796 |  |

## person_present

| option | n | recall | precision |
|---|---|---|---|
| false | 4719 | 0.896 | 0.822 |
| true | 4276 | 0.786 | 0.873 |

Confusion (rows = truth, columns = prediction): false, true

```
  4228     491
   915    3361
```

## animal_present

| option | n | recall | precision |
|---|---|---|---|
| false | 7808 | 0.864 | 0.962 |
| true | 1710 | 0.843 | 0.575 |

Confusion (rows = truth, columns = prediction): false, true

```
  6745    1063
   269    1441
```

## vehicle_present

| option | n | recall | precision |
|---|---|---|---|
| false | 7622 | 0.924 | 0.946 |
| true | 1755 | 0.771 | 0.699 |

Confusion (rows = truth, columns = prediction): false, true

```
  7039     583
   402    1353
```

## subject

| option | n | recall | precision |
|---|---|---|---|
| none | 177 | 0.429 | 0.439 |
| person | 2916 | 0.688 | 0.818 |
| animal | 1294 | 0.831 | 0.652 |
| vehicle | 1359 | 0.680 | 0.791 |
| food | 399 | 0.689 | 0.415 |
| tableware | 182 | 0.126 | 0.338 |
| furniture | 1210 | 0.531 | 0.571 |
| electronics | 292 | 0.568 | 0.599 |
| appliance | 312 | 0.644 | 0.400 |
| sports | 226 | 0.332 | 0.366 |
| bag | 177 | 0.209 | 0.294 |
| street | 371 | 0.466 | 0.367 |
| household | 292 | 0.387 | 0.345 |

Confusion (rows = truth, columns = prediction): none, person, animal, vehicle, food, tableware, furniture, electronics, appliance, sports, bag, street, household

```
    76       2      11       7       4       0       5       5      16       5       0      23      23
    19    2006     235     101      63       0     168      17      35      83      36     101      52
     7      54    1075      32      20       0      29      14      14       5       6      29       9
    23      94      86     924      18       1      27      16      24      20      17      79      30
     0       2      16       2     275      15      62       3       6       0       3       4      11
     0       4      10       4      60      23      48       1      19       0       0       2      11
     4      83      82       9     145      21     643      22     127       2      13      17      42
     4      16      15       4       3       1      43     166      24       0       4       6       6
     3      15      11       4      10       2      48       7     201       0       0       5       6
     8      83      19      13       5       0       1       5       2      75       3       7       5
     1      37      16      24       6       0      12       7      14       2      37      11      10
    16      35      44      35      16       0      12       4      11      12       3     173      10
    12      21      29       9      37       5      28      10       9       1       4      14     113
```

## animal_type

| option | n | recall | precision |
|---|---|---|---|
| none | 7808 | 0.902 | 0.954 |
| cat | 301 | 0.635 | 0.364 |
| dog | 254 | 0.303 | 0.292 |
| bird | 136 | 0.390 | 0.256 |
| horse | 203 | 0.512 | 0.421 |
| farm | 237 | 0.460 | 0.466 |
| wild | 579 | 0.798 | 0.704 |

Confusion (rows = truth, columns = prediction): none, cat, dog, bird, horse, farm, wild

```
  7045     254     124     115      93      60     117
    72     191      19       7       1       3       8
    81      53      77      11       9      13      10
    56      10       3      53       1       8       5
    53       2       5       0     104      13      26
    42       6      21       4      27     109      28
    36       9      15      17      12      28     462
```

## vehicle_type

| option | n | recall | precision |
|---|---|---|---|
| none | 7622 | 0.953 | 0.933 |
| car | 381 | 0.451 | 0.381 |
| bus | 268 | 0.526 | 0.644 |
| truck | 217 | 0.217 | 0.439 |
| motorbike | 202 | 0.569 | 0.578 |
| bicycle | 123 | 0.203 | 0.352 |
| train | 244 | 0.709 | 0.706 |
| boat | 172 | 0.529 | 0.576 |
| airplane | 148 | 0.615 | 0.632 |

Confusion (rows = truth, columns = prediction): none, car, bus, truck, motorbike, bicycle, train, boat, airplane

```
  7260     156      23      10      42      21      29      48      33
   150     172      10      19      10       6       8       3       3
    38      42     141      20       3       3      16       1       4
    80      51      23      47       2       1       7       4       2
    60       7       1       2     115      12       2       3       0
    56      12       4       0      22      25       3       1       0
    36       7      16       7       0       2     173       3       0
    57       3       0       1       3       0       6      91      11
    46       1       1       1       2       1       1       4      91
```

## indoors

| option | n | recall | precision |
|---|---|---|---|
| false | 5513 | 0.931 | 0.986 |
| true | 1674 | 0.956 | 0.807 |

Confusion (rows = truth, columns = prediction): false, true

```
  5131     382
    74    1600
```

## place

| option | n | recall | precision |
|---|---|---|---|
| other | 3947 | 0.637 | 0.849 |
| kitchen | 237 | 0.650 | 0.368 |
| bathroom | 375 | 0.763 | 0.571 |
| bedroom | 232 | 0.448 | 0.488 |
| living_room | 227 | 0.586 | 0.399 |
| eating | 57 | 0.105 | 0.176 |
| office | 185 | 0.514 | 0.621 |
| street | 1064 | 0.728 | 0.602 |
| green | 876 | 0.847 | 0.554 |
| nature | 417 | 0.602 | 0.751 |
| water | 547 | 0.739 | 0.689 |

Confusion (rows = truth, columns = prediction): other, kitchen, bathroom, bedroom, living_room, eating, office, street, green, nature, water

```
  2516     138     133      72      99      27      44     388     394      44      92
    26     154      19       5      29       0       2       2       0       0       0
    40      24     286       5       8       0       4       5       2       1       0
    58      18      19     104      24       0       3       3       2       0       1
    30      31      10      16     133       0       4       1       2       0       0
    21      11       3       0      10       6       0       6       0       0       0
    37      17      12       4      19       0      95       0       1       0       0
   114      21      11       6       6       1       0     775      82      20      28
    56       2       2       1       0       0       0      45     742       3      25
    33       1       3       0       0       0       0      32      61     251      36
    34       1       3       0       5       0       1      30      54      15     404
```

## weather

| option | n | recall | precision |
|---|---|---|---|
| none | 987 | 0.973 | 0.942 |
| clear | 625 | 0.739 | 0.845 |
| cloudy | 651 | 0.636 | 0.726 |
| rain | 192 | 0.693 | 0.454 |
| snow | 187 | 0.727 | 0.680 |
| fog | 126 | 0.786 | 0.712 |

Confusion (rows = truth, columns = prediction): none, clear, cloudy, rain, snow, fog

```
   960       8       8       4       5       2
    20     462      93      33      15       2
    22      64     414      92      34      25
     5       5      31     133       9       9
     5       8      14      22     136       2
     7       0      10       9       1      99
```
