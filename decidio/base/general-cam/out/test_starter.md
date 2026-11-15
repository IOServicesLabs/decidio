# general-cam - test on the starter set (int8)

| question | n | accuracy | majority baseline | int8 accuracy | notes |
|---|---|---|---|---|---|
| person_present | 1810 | 0.855 | 0.529 | 0.855 | AUC 0.929, best F1 0.844 at p>0.45 |
| animal_present | 1913 | 0.850 | 0.826 | 0.850 | AUC 0.926, best F1 0.750 at p>0.80 |
| vehicle_present | 1877 | 0.906 | 0.839 | 0.906 | AUC 0.950, best F1 0.752 at p>0.65 |
| subject | 1848 | 0.613 | 0.312 | 0.613 |  |
| animal_type | 1913 | 0.834 | 0.826 | 0.834 |  |
| vehicle_type | 1877 | 0.874 | 0.839 | 0.874 |  |
| indoors | 0 | | | | no labeled validation samples |
| place | 0 | | | | no labeled validation samples |
| weather | 0 | | | | no labeled validation samples |

## person_present

| option | n | recall | precision |
|---|---|---|---|
| false | 958 | 0.896 | 0.840 |
| true | 852 | 0.809 | 0.873 |

Confusion (rows = truth, columns = prediction): false, true

```
   858     100
   163     689
```

## animal_present

| option | n | recall | precision |
|---|---|---|---|
| false | 1581 | 0.855 | 0.960 |
| true | 332 | 0.831 | 0.545 |

Confusion (rows = truth, columns = prediction): false, true

```
  1351     230
    56     276
```

## vehicle_present

| option | n | recall | precision |
|---|---|---|---|
| false | 1574 | 0.922 | 0.964 |
| true | 303 | 0.822 | 0.669 |

Confusion (rows = truth, columns = prediction): false, true

```
  1451     123
    54     249
```

## subject

| option | n | recall | precision |
|---|---|---|---|
| none | 41 | 0.512 | 0.512 |
| person | 577 | 0.685 | 0.816 |
| animal | 253 | 0.834 | 0.655 |
| vehicle | 230 | 0.700 | 0.719 |
| food | 102 | 0.627 | 0.471 |
| tableware | 37 | 0.081 | 0.150 |
| furniture | 232 | 0.474 | 0.529 |
| electronics | 57 | 0.509 | 0.518 |
| appliance | 73 | 0.562 | 0.394 |
| sports | 56 | 0.411 | 0.418 |
| bag | 34 | 0.147 | 0.208 |
| street | 97 | 0.464 | 0.425 |
| household | 59 | 0.424 | 0.368 |

Confusion (rows = truth, columns = prediction): none, person, animal, vehicle, food, tableware, furniture, electronics, appliance, sports, bag, street, household

```
    21       0       1       0       0       1       1       0       4       1       0       8       4
     1     395      44      19      19       0      37       2       3      19       7      17      14
     2      11     211       3       3       0       5       5       4       2       0       4       3
     3      18       8     161       1       0       3       5       4       4       7      15       1
     0       3       4       0      64       9      13       0       4       0       0       3       2
     0       1       5       3      10       3      10       0       3       0       0       0       2
     1      20      16       3      26       6     110       8      27       1       1       4       9
     2       3       3       1       1       1       8      29       7       0       1       0       1
     3       6       4       2       2       0      10       0      41       0       0       3       2
     3      11       5       6       1       0       0       1       1      23       1       2       2
     0       4       2      10       0       0       3       2       1       1       5       3       3
     5       8      10      14       4       0       4       2       1       3       1      45       0
     0       4       9       2       5       0       4       2       4       1       1       2      25
```

## animal_type

| option | n | recall | precision |
|---|---|---|---|
| none | 1581 | 0.896 | 0.954 |
| cat | 51 | 0.490 | 0.240 |
| dog | 47 | 0.298 | 0.222 |
| bird | 17 | 0.412 | 0.194 |
| horse | 42 | 0.452 | 0.328 |
| farm | 54 | 0.259 | 0.483 |
| wild | 121 | 0.826 | 0.725 |

Confusion (rows = truth, columns = prediction): none, cat, dog, bird, horse, farm, wild

```
  1417      62      29      21      24       8      20
    21      25       5       0       0       0       0
    12      14      14       4       1       1       1
     7       0       1       7       1       1       0
    11       1       2       0      19       1       8
     9       1       9       0      12      14       9
     8       1       3       4       1       4     100
```

## vehicle_type

| option | n | recall | precision |
|---|---|---|---|
| none | 1574 | 0.950 | 0.947 |
| car | 71 | 0.507 | 0.400 |
| bus | 40 | 0.450 | 0.486 |
| truck | 43 | 0.256 | 0.524 |
| motorbike | 31 | 0.613 | 0.475 |
| bicycle | 21 | 0.190 | 0.286 |
| train | 37 | 0.486 | 0.562 |
| boat | 31 | 0.613 | 0.500 |
| airplane | 29 | 0.655 | 0.760 |

Confusion (rows = truth, columns = prediction): none, car, bus, truck, motorbike, bicycle, train, boat, airplane

```
  1496      32       4       3       9       6       5      14       5
    25      36       4       3       2       1       0       0       0
    11       3      18       1       2       0       4       1       0
    11      13       6      11       0       0       2       0       0
     7       1       0       0      19       2       1       1       0
     7       2       0       0       7       4       0       1       0
     7       3       4       2       0       1      18       2       0
     9       0       0       0       1       0       1      19       1
     7       0       1       1       0       0       1       0      19
```
