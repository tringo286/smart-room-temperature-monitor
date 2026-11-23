# 3-minute presentation script

Smart Room Temperature Monitor, CMPE 286: seven slides, about 180 seconds.
Total: 419 words, which is about 3 minutes at a steady 140 words per minute. The same text is in each slide's speaker notes in the deck.

Tips: practise with a timer, and pause briefly on the forecast and testing numbers. If you run long, shorten slide 4 first.

## Slide 1: Title [0:00–0:15, about 15 seconds]

Hi, we’re Duy Lam and Tri. We built a Smart Room Temperature Monitor: a cheap device that warns people before their room gets too hot, and keeps working when the Wi-Fi or server goes down.

*35 words*

## Slide 2: The problem [0:15–0:40, about 25 seconds]

Dorm rooms, labs and server closets often overheat, and nobody notices until it’s uncomfortable or equipment is at risk. Smart thermostats are locked to a vendor cloud.
So we set four goals: measure constantly, warn the room right away, check it from any phone, and survive network failures.

*48 words*

## Slide 3: Architecture [0:40–1:15, about 35 seconds]

On the left, the NodeMCU reads the DHT11 every two seconds. Our key decision: the alarm runs on the board itself, so the LCD and buzzer work with no Wi-Fi and no server.
Readings go over MQTT to Mosquitto; Telegraf stores them in InfluxDB, and Grafana shows them. Commands flow back the same way, and the MQTT Last Will reports when the device drops off. The whole server starts with one Docker Compose command.

*74 words*

## Slide 4: What people see [1:15–1:40, about 25 seconds]

People get three views. In the room, the LCD shows the reading and the buzzer sounds at the threshold. Any phone on the network can open the board’s own web page, with live gauges, a 20-minute chart, and threshold and mute controls, no internet needed. And Grafana keeps the full history.

*51 words*

## Slide 5: The forecast [1:40–2:15, about 35 seconds]

Our improvement is a forecast. A small service fits a trend to the last 15 minutes and predicts 10 minutes ahead. It sends the time to the alert back to the board, so the LCD says “Alert in about 6 minutes” before the buzzer sounds.
It grades itself against what really happens. In a simulated heat event it warned 16 to 18 minutes early, with an error of 0.9 degrees, against 2.7 for assuming nothing changes.

*76 words*

## Slide 6: Testing and results [2:15–2:45, about 30 seconds]

Without constant access to the hardware, we ran the real firmware on a laptop against fake hardware: 26 tests, plus 14 for the forecast. They caught a real bug: with the server off, the board was frozen 99 % of the time; now it’s 11 %.
An end-to-end test of the live stack passes all 51 checks, and it caught another bug, in the forecaster, which we fixed. Data reaches the database in under 5 seconds, a crashed device shows offline instantly, and the firmware uses 39 % of the ESP8266’s RAM.

*93 words*

## Slide 7: Takeaway [2:45–3:00, about 15 seconds]

To sum up: the alarm runs on the board, so the room stays protected when the network fails, and the forecast warns ahead of time. Next: validate on the real board, add fan control, push alerts and secure MQTT. Thank you! Questions?

*42 words*
