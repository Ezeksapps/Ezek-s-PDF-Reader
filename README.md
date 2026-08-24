# Ezek's PDF Reader v0.2

NOTE: OLD PROJECT
(also very badly programmed project)

This was made back in late 2023 and last updated sometime in 2024
(I cannot give any proper exact dates, since this was back when I used Windows
The partition holding that install was on my old SSD which has since been wiped twice due to
attempts to install Linux and fully died august 2025)

The code base for version 0.2 uses Boost regex to parse PDF files
Looking back, it is very obvious to me that regex is a terrible way of doing such a thing
This library is also severely lacking in features and is more an experiment than anything else

No one seriously attempt to use this in their own projects, please... unless it's also just a messy personal experiment

## Current Features

* Can properly extract text from PDFs
* Can also read & decode images
* Decodes font data

## Known Issues

* Still very simple. Doesn't support linearised PDFs

(See the comment in the include header for more details)
