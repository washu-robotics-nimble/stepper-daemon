# Description

This project offers API to drive a standard NEMA17 stepper motor using a A4988 driver.

Designed to run on a Raspberry Pi 4, and (should) support previous generations.



# Usage

```shell
# First get the necessary packages:

sudo apt update
sudo apt install gpiod libgpiod-dev
# You likely don't need this since we are using C:
# sudo apt install python3-gpiozero python3-lgpio python3-pip

# Then install the lg library:
wget https://github.com/joan2937/lg/archive/master.zip
unzip master.zip
cd lg-master
make
sudo make install

# Now compile this project:
make # compiles project.        # Cross-compiling needed if ran on a different machine.
nano motor.ini                  # Adjust parameters as needed.
./motor_demo                    # You probably need root access; see below for more info

# About root access:
# You probably need root access to run it, if you are on Ubuntu.
ls -la /dev/gpiochip*           # Your user may have --- permissions
gentent group gpio              # Check if gpio group exists
# sudo groupadd gpio            # Add it if it doesn't exist
sudo chown root:gpio /dev/gpiochip*  # Change group ownership of gpio devices
sudo usermod -aG gpio $USER     # Add yourself to the group
newgrp gpio                     # Apply without restart/logout
groups                          # Verify. Now it should work without root access

```

# Todo list

- [x] Minimum verification
- [ ] Motor driver core
    - [x] Basic movement support (abs, rel)
    - [x] Threaded program
    - [x] Acceleration limiting (trapezoidal ramp)
    - [ ] Endstop support
- [x] CLI-based control
    - [x] STDIN reading
    - [x] Real-time status display
- [ ] Remote control
    - [ ] Control over serial
    - [ ] Control over LAN

# Notes

- Since this project relies on the `lg` library it does not support Raspberry Pi 5 (and future generations).
    - Adaption to use `sysfs` interface will be needed.
