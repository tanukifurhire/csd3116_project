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

## Run

**Compile the project**
```bash
make all
```
This compiles the project with all the necessary tools, libraries and links with OpenGL/GLFW/DDS. 

**Run the project**
For Client:
```bash
./client
```

For Server: 
```bash
./server
```