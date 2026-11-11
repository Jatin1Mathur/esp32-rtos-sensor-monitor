# Dataset: real office-room temperature and humidity

`office_room_2015.csv` contains **2,665 real readings** (one per minute) of
temperature and relative humidity from an office room in Mons, Belgium,
recorded from 2015-02-02 14:19 to 2015-02-04 10:43.

| Column | Unit | Range in this file |
|---|---|---|
| `timestamp` | date and time | 2015-02-02 14:19 .. 2015-02-04 10:43 |
| `temperature_c` | °C | 20.20 .. 24.41 |
| `humidity_pct` | % relative humidity | 22.10 .. 31.47 |
| `occupancy` | 0 = empty, 1 = occupied | kept for reference, not used |

## Why it is in this project

I do not have an M5Stack / SHT3x sensor yet. Instead of made-up numbers, the
firmware (`pio run -e m5stack-replay`), the host tests and the demo client
replay these real measurements through the exact same tasks, queues and bridge.
The data has real-world behaviour: slow drifts, day/night changes and jumps
when people enter the room.

## Source and license

*Occupancy Detection* dataset, published on the UCI Machine Learning Repository
and also available on Kaggle. Taken from the `datatest.txt` file of the
original authors' repository.

* L. Candanedo, V. Feldheim (2016). *Accurate occupancy detection of an office
  room from light, temperature, humidity and CO2 measurements using statistical
  learning models.* Energy and Buildings, Vol. 112.
* Candanedo, L. (2016). *Occupancy Detection* [Dataset]. UCI Machine Learning
  Repository. https://doi.org/10.24432/C5X01N
* License: **Creative Commons Attribution 4.0 (CC BY 4.0)**

## How the files were made

```bash
python tools/prepare_dataset.py path/to/datatest.txt
```

The script keeps only temperature, humidity and occupancy, checks that all
values are inside the SHT3x measuring range and in time order, and writes
`data/office_room_2015.csv` plus `src/replay_data.h` (the same values as a C
array that the firmware stores in flash).
