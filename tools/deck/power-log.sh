#!/bin/bash
# Power log for a Steam Deck run: one line a second until killed.
#   power-log.sh <output file>
# Columns: ms_of_day (the clock of frames.log and stamp.py), battery status, battery watts, APU watts.
# Battery watts (current x voltage) are the whole system's draw only while the status is
# Discharging; on mains they are the charge rate. They follow the load about half a minute late.
# APU watts (amdgpu power1_average) are the CPU and GPU alone, on battery or mains.
BAT=/sys/class/power_supply/BAT1
APU=$(ls /sys/class/drm/card*/device/hwmon/hwmon*/power1_average 2>/dev/null | head -1)
echo "# ms_of_day battery_status battery_w apu_w" > "$1"
while sleep 1; do
    awk -v t="$(date '+%-H %-M %-S %N')" -v status="$(tr ' ' '_' < $BAT/status)" \
        -v ua="$(< $BAT/current_now)" -v uv="$(< $BAT/voltage_now)" -v apu="$(< "$APU")" \
        'BEGIN { split(t, c, " "); if (ua < 0) ua = -ua
                 printf "%d %s %.2f %.2f\n", ((c[1] * 60 + c[2]) * 60 + c[3]) * 1000 + c[4] / 1e6, status, ua * uv / 1e12, apu / 1e6 }' >> "$1"
done
