# ContextChaos: Preemptive User-Space Fiber Runtime

## Overview
This repository contains a high-performance, preemptive user-space fiber scheduler written in C and x86-64 assembly. It successfully handles 50,000+ concurrent fibers, asynchronous I/O via `epoll`, priority-based mutexes, and strict System V ABI compliance. 

## 🛠 Build Instructions (Clean Checkout)
This project is built to compile and execute inside the official `ostrack:core` Docker environment.

1. **Launch the Docker Container:**
   Mount this repository into the container with the necessary tracing capabilities:
   ```bash
   docker run -it --rm --cap-add=SYS_PTRACE --security-opt seccomp=unconfined -v "$PWD":/work ostrack:core
   ```

2. **Navigate to the Source Directory:**
   ```bash
   cd /work
   ```
   *(If your repository contents are in a subfolder, navigate to that folder).*

3. **Build the Project:**
   Build the static library (`libcontextchaos.a`) from a clean state:
   ```bash
   make clean
   make
   ```

## 🎥 Submission Videos
* **Recorded Demo (Max 5 mins):** https://drive.google.com/file/d/1BSmdcngcjYtZ6qAOiGzNYumNFVYk8tQZ/view?usp=drive_link
* **Unedited Public Test Run:** https://drive.google.com/file/d/1BSmdcngcjYtZ6qAOiGzNYumNFVYk8tQZ/view?usp=drive_link

## 📦 Problem-Specific Artifacts Included
* `libcontextchaos.a` - Prebuilt static library binary (Fallback).
* `Architecture_Spec.md` - Design document detailing stack layout, register handling, and signal safety.
* `fiber.c` - Core C runtime and scheduler logic.
* `context_switch.s` - Custom x86-64 assembly context switching.
* `Makefile` - Build instructions.
* `include/fiber.h` - Unmodified standard header.

## 🚀 Running Tests and Benchmarks
**Public Tests (16/16 Passing):**
```bash
/opt/os-track/contextchaos/tests/run_public.sh libcontextchaos.a
```
**Benchmarks:**
```bash
/opt/os-track/contextchaos/bench/run_bench.sh libcontextchaos.a
```
