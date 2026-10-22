### Building the firmware

# Building the firmware

Run this in the root directory
```bash
./build.sh
```

## Update using web interface
1. Go to `http://<sensor ip>/update`
2. Enter username and password set in the secrets.h file.
3. Select the bin file and upload it.

## Flash firmware over USB

# To find the device serial connection
```bash
ls /dev/cu.*
```

# Upload the new binary to the sensor
```bash
cd build-docker/
python3 -m esptool --chip esp32p4 -p /dev/cu.usbmodem5B8F0858201 -b 460800 --before default_reset --after hard_reset write_flash "@flash_args"
screen /dev/cu.usbmodem5B8F0858201 115200
```

