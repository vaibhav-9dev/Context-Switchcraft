# ContextChaos: User-Space Fiber Runtime

A highly scalable, preemptive user-space fiber runtime written in C and x86-64 assembly. Designed for the OS Track Hackathon, this scheduler supports 50,000+ concurrent fibers, asynchronous I/O via `epoll`, priority-based mutexes, and strict System V ABI compliance.

## 📦 Problem-Specific Artifacts Included
* `libcontextchaos.a` - Prebuilt static library binary.
* `Architecture_Spec.md` - Technical design document detailing stack layout, register handling, and signal safety.
* `fiber.c` - Core C runtime and scheduler logic.
* `context_switch.s` - x86-64 assembly context switching.
* `Makefile` - Build instructions.
* `include/fiber.h` - Unmodified standard header.

## 🎥 Submission Videos
* **Recorded Architecture & Benchmark Demo (Max 5 mins):** https://drive.google.com/file/d/1BSmdcngcjYtZ6qAOiGzNYumNFVYk8tQZ/view?usp=sharing
* **Unedited Public Test Run (13/13 Passes):** https://drive.google.com/file/d/1BSmdcngcjYtZ6qAOiGzNYumNFVYk8tQZ/view?usp=sharing

## 🛠 Build Instructions (Clean Checkout)
This project is built to compile and execute exclusively inside the official `ostrack:core` Docker environment. 

**1. Launch the Docker Container**
Mount this repository into the container with the necessary tracing capabilities:
```bash
docker run -it --rm --cap-add=SYS_PTRACE --security-opt seccomp=unconfined -v "$PWD":/work ostrack:core
