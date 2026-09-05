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

**Prerequisites** (in addition to CycloneDDS/GLFW): the server hashes player passwords with OpenSSL, so it needs the OpenSSL development headers:
```bash
sudo apt install libssl-dev
```

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