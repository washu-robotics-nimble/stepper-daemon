# Description

This project offers API to drive a standard NEMA17 stepper motor using a A4988 driver.

Designed to run on a Raspberry Pi 4, and (should) support previous generations.



# Usage

```
# First install the lg library:
wget https://github.com/joan2937/lg/archive/master.zip
unzip master.zip
cd lg-master
make
sudo make install

# Then compile this project:
make # compiles project.        # Cross-compiling needed if ran on a different machine.
nano motor.ini                  # Adjust parameters as needed.
./motor_demo
```

# Todo list

- [x] Minimum verification
- [ ] Motor driver core
    - [x] Basic movement support (abs, rel)
    - [x] Threaded program
    - [ ] Endstop support
- [ ] CLI-based control
    - [ ] STDIN reading
    - [ ] Real-time status display
- [ ] Remote control
    - [ ] Control over serial
    - [ ] Control over LAN



# Notes

- Since this project relies on the `lg` library it does not support Raspberry Pi 5 (and future generations).
    - Adaption to use `sysfs` interface will be needed.