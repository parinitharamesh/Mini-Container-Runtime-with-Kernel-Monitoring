# Mini Container Runtime with Kernel Monitoring

## 📌 Overview

This project implements a lightweight container runtime in C using Linux system calls.
It provides process isolation, filesystem isolation, logging, and kernel-level monitoring.

The goal of this project is to understand how containers (like Docker) work internally by building a simplified version from scratch.

---

## 🎯 Objectives

* Create containers using Linux system calls
* Provide process and filesystem isolation
* Manage multiple containers (start, stop, list)
* Capture container logs
* Integrate kernel module for monitoring

---

## ⚙️ System Architecture

The system follows this flow:

User → Engine → Container → Kernel → Monitor Module

* **Engine (engine.c)**: Handles user commands
* **Container**: Isolated environment created using clone()
* **Kernel**: Provides isolation using namespaces
* **Kernel Module**: Monitors container processes

---

## 🧠 Concepts Used

* **clone()** → Creates container process
* **Namespaces** → Isolate processes and system resources
* **chroot()** → Isolate filesystem
* **mount()** → Mount `/proc` for process visibility
* **execvp()** → Runs program inside container
* **pipe() + dup2()** → Capture logs
* **ioctl()** → Communicate with kernel module

---

## ✨ Features

* Container creation using clone()
* Process isolation using namespaces
* Filesystem isolation using chroot()
* Logging using pipes and file redirection
* Kernel module integration for monitoring
* Container management commands:

  * `start`
  * `ps`
  * `stop`
  * `logs`

---

## 📁 Project Structure

```
OS-Jackfruit/
│
├── engine.c
├── monitor.c
├── monitor_ioctl.h
├── Makefile
│
├── test_programs/
│   ├── cpu_hog.c
│   ├── memory_hog.c
│   ├── io_pulse.c
│
├── outputs/
│   ├── alpha.log
│   ├── screenshots/
│
├── report.pdf
├── presentation.pdf
└── README.md
```

---

## 🚀 How to Run

### 1. Compile

```
make
```

### 2. Start Container

```
sudo ./engine start alpha ../rootfs-alpha /bin/sh
```

### 3. Load Kernel Module

```
sudo insmod monitor.ko
```

### 4. Verify Module

```
lsmod | grep monitor
```

### 5. Check Device

```
ls /dev/container_monitor
```

### 6. List Containers

```
./engine ps
```

### 7. Stop Container

```
./engine stop alpha
```

### 8. View Logs

```
./engine logs alpha
```

---

## 📊 Sample Output

* Container created with PID
* Logs stored in `<container_name>.log`
* Containers listed using `ps` command

---

## ⚠️ Limitations

* No advanced networking support
* Basic resource management
* Simplified implementation compared to Docker

---

## 🧾 Conclusion

This project demonstrates core operating system concepts such as:

* Process isolation
* Filesystem isolation
* Inter-process communication
* Kernel-user interaction

---

## 👩‍💻 Contributors

* Pallavi J
* Parinitha Ramesh

---

## 📚 References

* Linux Man Pages
* Operating Systems Concepts
* Online documentation for system calls
