# AirCube Menu Bar Setup

To set up the AirCube Menu Bar application, follow the instructions below:

**1.** Ensure you have Python 3 installed on your system.

**2.** Install the required dependencies by running:

```
pip install rumps pyserial pyinstaller
```

**3.** Save the `aircube_menubar.py` script to your desired location.

**4.** Open a terminal and navigate to the directory where you saved the `aircube_menubar.py` script.

**5.** Run the following command to create a standalone application using PyInstaller:\*\*

```
pyinstaller \
	--windowed \
	--name "AirCube Menu Bar" \
	aircube_menubar.py
```

**6.** After the build process is complete, you will find the generated application in the `dist` directory.

**7.** Move the generated application to your Applications folder or any desired location.

**8.** Launch the AirCube Menu Bar application, and it should appear in your menu bar, allowing you to interact with it as needed.

**9.** Add the AirCube Menu Bar application to your startup items to ensure it launches automatically when you log in.

**Note:** If you encounter any issues during the setup process, please refer to the documentation or seek assistance from the AirCube community.
