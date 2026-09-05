# CSD3116 Low Level Programming - Group Assignment

---

## Setup

**Windows**
- Clone the repository in GitHub Desktop
- This project only runs in Linux, so you'll have to use WSL (see [run](#run))

**Linux**
To clone the repository: 

HTTPS:
```bash
git clone https://github.com/tanukifurhire/csd3116_project.git
```

SSH:
```bash
git clone git@github.com:tanukifurhire/csd3116_project.git
```
*(This requires you to have already generated an SSH key in WSL and added it to your GitHub profile.)*

### Prerequisites

Run all of these inside WSL2 (Ubuntu), not in Windows.

1. **Open WSL2.**

2. **Install the OpenGL development files.**
   ```bash
   sudo apt install libgl1-mesa-dev
   ```

3. **Graphics Library Framework (GLFW)** [1] shall be used for windowing and keyboard input.

4. **Install the GLFW headers and libraries.**
   ```bash
   sudo apt install libglfw3-dev
   ```

5. **Install GLEW.** The C++ client renders with the OpenGL 3.3 core profile
   (shaders, VAOs, VBOs); those entry points are not exported by libGL directly
   and GLEW [3] loads them at runtime.
   ```bash
   sudo apt install libglew-dev
   ```

6. **Cyclone DDS** [2] shall be used for all networking — install with:
   ```bash
   sudo apt update
   sudo apt install -y cyclonedds-dev cyclonedds-tools
   ```
   `cyclonedds-dev` provides the headers and the `CycloneDDS.pc` file that the
   makefile looks up via `pkg-config`; `cyclonedds-tools` provides `idlc`, the
   IDL compiler that turns `messages.idl` into `messages.c`/`messages.h`.

7. **Install the OpenSSL development headers.** The server hashes player
   passwords with OpenSSL, and the DDS Security plugins need it too.
   ```bash
   sudo apt install libssl-dev
   ```

To confirm everything is in place before building:
```bash
idlc --version
pkg-config --exists CycloneDDS && echo "CycloneDDS OK"
pkg-config --exists glfw3 && echo "GLFW OK"
pkg-config --exists glew && echo "GLEW OK"
```

### References

- [1] GLFW — https://www.glfw.org/
- [2] Eclipse Cyclone DDS — https://cyclonedds.io/
- [3] GLEW — https://glew.sourceforge.net/

## Run

**Compile the project**
```bash
make all
```
This compiles the project with all the necessary tools, libraries and links with OpenGL/GLFW/DDS. 

**Run the project**
For Client:
```bash
./client       # or ./client N for client cert N (1-4)
```
You'll be asked for a player name and password. A new name registers that password; an existing name must match the one it registered with.

For Server: 
```bash
./server
```
The server stores each account's salted password hash in `players.auth` (created next to the binary, `chmod 600`, never committed) so registered players are remembered across restarts.

**Or run a full local demo in one shot**
```bash
make run N=3   # starts the server, then N clients (1-4)
```