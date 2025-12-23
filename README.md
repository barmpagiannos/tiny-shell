# TinyShell

A Unix-style shell written in **C**, implementing command execution with  
**PATH search**, **I/O redirection**, **pipelines**, and **job control**.

---

## Overview

**TinyShell** is an educational Unix shell developed as part of the  
**Operating Systems** course (9th semester).

The project follows a **3-phase incremental design**, evolving from a basic
command executor into a fully–featured shell with job control and signal
handling.

**Author:** Vasilis Barmpagiannos  
**Language:** C  
**Platform:** Linux / WSL  

---

## Project Phases

### Phase 1 — Command Execution
- `fork` / `execvp` / `wait`
- PATH search
- Exit status reporting
- Built-in command: `exit`

### Phase 2 — I/O Redirection & Pipelines
- Input redirection: `<`
- Output redirection: `>`
- Append redirection: `>>`
- Pipelines: `|`
- Quote handling

### Phase 3 — Job Control & Signals
- Background execution: `&`
- Job management: `jobs`, `fg`, `bg`
- Process groups
- Signal handling (`SIGINT`, `SIGTSTP`, `SIGCHLD`)
- Built-in command: `cd`

---

## Build & Installation

### Prerequisites
- Linux environment (Ubuntu, Debian, WSL, etc.)
- GCC compiler

### Compilation
```bash
gcc -o tinyshell_final tinyshell_final.c -Wall -Wextra
