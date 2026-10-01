# Sotek's Spite

Sotek's Spite is a C-based toolkit for making Dead Rising 3 super moddable. The game can be forced to read loose file mods, meaning no need for a mod manager to handle the .big archives. I highly suggest reading Sotek's_Guide.txt in the Guide folder before using especially if you're lookin to make mods because you will want to know about the unpack depth level (what the GUI calls Unpack mode).

# For windows

Look I only have a windows 10 PC, so Sotek's Spite assumes you're on windows 10/11 since I'm using win32 and GDI+. If you're on linux, you might be able to get Sotek's Spite to work by using wine but I can't test Sotek's Spite on an OS I don't own.

# Controls for using Sotek's Spite

Hold right click over the GUI to drag the app and any windows that popup such as the error triangle (what is used if an error occurs)

You can click the Esc button to exit the app or just click the Exit button at the bottom

# More details

The toolkit can patch the game to read loose files present in a Mods folder. Modded files can be smaller, the same size, or larger. That means dynamic file sizes are supported. You're not restricted to maintaining the original file size.

The modding workflow is pretty damn easy. You place dinput8.dll, SotekSpite.exe, and vanilla_companions.txt in the game folder next to deadrising3.exe. If you just want to copy from my SotekSpite.ini then just place the Mods folder bundled with this repository at steamapps\common\deadrising3\Mods. Then you run SotekSpite.exe (if you need to unpack files, build new .big archives, update files, etc), mod whatever the fuck you want, place modded files in the Mods folder (i.e., would be at steamapps\common\deadrising3\Mods), and then play the damn game! This is a brief explanation, for more details you may want to read Sotek's_Guide.txt in the Guide folder for the 2 ways of applying mods to DR3.

If you need to mod files stored within .big archives, you're covered! Sotek's Spite can unpack .big files as well as create new .big archives.

Sotek's Spite is inspired by Sotek from warhammer. I'm going to be very clear, this project isnt endorsed by Games Workshop and if they come across this repository and want it renamed then i'll do it. The toolkit merely uses the name "Sotek" to make the toolkit sound rad but also to make people curious about lizardmen, which hopefully then leads them to become warhammer fans like myself. Lizardmen stay winning!

# GUI sample

This shows an example of the GUI, it's intentionally designed to be unique and unlike other software for modding

<img width="1140" height="739" alt="s1" src="https://github.com/user-attachments/assets/020b569d-d329-42f6-9829-373c34b0e119" />

<img width="1915" height="1035" alt="s2" src="https://github.com/user-attachments/assets/69322c4b-9686-4201-b73e-add8f3eb2571" />

# Basic mod example I did

<img width="1000" height="270" alt="s3" src="https://github.com/user-attachments/assets/a81ef3eb-7023-4073-b121-fbb00fbbc260" />

<img width="1280" height="720" alt="modded2" src="https://github.com/user-attachments/assets/dc652fce-0736-470b-8366-601d9bfc8a87" />
