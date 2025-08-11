# dwl - dwm for Wayland

> ### ⚠️ Migrated to Codeberg
>
> This project has [migrated to Codeberg](https://codeberg.org/QuantumWhaleX11/anifetch).
This is a command-line tool that displays video as ASCII art directly in your terminal, alongside system information fetched by `fastfetch`.

## Dependencies

To build and run the program, you'll need the following components:

### For Building from Source:

*   **C++23 Compliant Compiler:** A compiler that supports C++23 standards.
*   **Standard C++ Libraries:** The build relies on standard libraries typically included with your C++23 compiler distribution.
*   **`pthread` Library:** For multi-threading support. This is usually available by default on Linux/macOS systems and is enabled by the `-pthread` compiler flag specified in the `Makefile`.

### For Running the Program:

These external command-line tools must be installed on your system and be accessible via your system's `PATH` environment variable:

*   **`ffmpeg`:** Essential for all video processing tasks, including:
    *   Decoding video files.
    *   Extracting individual frames as PNG images.
    *   Extracting audio streams from video files.
    *   Probing video/audio file information (duration, codecs).
*   **`chafa`:** The core utility for converting image frames (PNGs) into ASCII or other symbol-based character art for terminal display.
*   **`fastfetch`:** Used to generate the system information that is displayed alongside the ASCII animation.

Ensure these tools are correctly installed and configured on your system.

## Building

1.  **Clone the repository:**
    ```bash
    git clone https://github.com/QuantumWhaleX11/anifetch.git
    cd anifetch
    ```

2.  **Build the executable:**
    
    Run `make` in the project directory. This will compile the code and create a binary executable file named `anifetch` in the project root.
    ```bash
    make
    ```

    Alternatively, you can run `make install`. This does the same thing but also prints a confirmation message.

The above steps generate a **C++ binary executable** for usage named `anifetch`.

## Usage

```bash
./anifetch --file <video> [options] [chafa_options]
```

This program accepts arguments for `chafa`. Any flag starting with `--` that is not a key option will be passed directly to the `chafa` command.

### Key Options:

*   `--file <path>`: **(Required)** Path to the input video file. It is **recommended** to put the video file in the same directory as the C++ binary executable.
*   `--size <WxH>`: A shortcut to set width (default: 80) and height (default: 24) at once (e.g., `--size 80x24`).
*   `--framerate <int>`: Framerate for extracting and animating frames (default: 30 fps).
*   `--sound [path_to_audio_file]`: Enables audio. If `[path_to_audio_file]` is provided, that file is used. If no path is given, extracts audio from the input video. It is **recommended** to put the sound file in the same directory as the C++ binary executable.
*   `--chroma <0xRRGGBB>`: Enables chroma keying. Removes pixels matching the specified hex color (e.g., `0x00FF00` for green).
*   `--debug: Enables detailed output, useful for debugging the asset pipeline.

### Direct Chafa Arguments:

You can pass any `chafa` flag directly.

Some common Chafa flags include:
*   `--symbols <symbol-list>`: Sets the character symbols to use (e.g., `ascii`, `block`, `wide`, `all`).
*   `--fg-only`: Only uses foreground colors, leaving the background transparent.
*   `--bg-only`: Only uses background colors.
*   `--fill <symbol-list>`: Fills empty background space with the given symbols.
*   `--stretch`: Stretches the image to fill the specified size, ignoring aspect ratio.

**Argument Precedence:** Last-to-appear precedence holds true for all clashing arguments, such as `--fg-only` and `--bg-only`. For example, in `./anifetch --file your_clip.mp4 --sound --fg-only --bg-only`, `--bg-only` takes precedence as --bg-only is last-to-appear.

If you provide an argument that `chafa` does not recognize, the rendering process will fail and an error will be displayed.

### Examples (Assuming video files and/or sound files are put in the same directory as the C++ executable) :

Play a clip with sound, using wide symbols and a specific framerate

```bash
./anifetch --file your_clip.mp4 --size 60x30 --sound --framerate 60 --symbols wide
```

or use a custom sound file, a high framerate, and only render foreground characters

```bash 
./anifetch --file your_clip.mkv --size 100x50 --sound your_sound.wav --framerate 144 --fg-only
```

or green-screen removal and fill empty space with dots 

```bash
./anifetch --file greenscreen.mp4 --chroma 0x00FF00 --fill "·."
```

and various other arguments.

## Caching

This program implements a caching system to speed up subsequent runs with the same video and parameters.

*   **Cache Location:** A base directory named `.cache/` is created in the project's root directory. Inside, a subdirectory is created for each video file, named after the video's filename (e.g., `.cache/your_clip.mp4/`).
*   **Cache Structure:**
    *   The video-specific directory (e.g., `.cache/your_clip.mp4/`) contains:
        *   `template.txt`: The static layout text generated from `fastfetch` output.
        *   **Hash-specific subdirectories:** For each unique set of processing arguments (input video identity, dimensions, framerate, chroma key, sound settings, and all Chafa options), a subdirectory is created using a hash of these parameters. This allows different visual styles for the same video to be cached separately. This hash directory stores:
            *   `cache.txt`: A file storing the metadata and arguments used for this specific cached version.
            *   `frames.dat`: A single file containing the generated ASCII frames concatenated together.
            *   `frames.idx`: An index file that maps each frame to its position and size within `frames.dat`.
            *   The extracted or copied sound file (if `--sound` was used).

## License

This project is licensed under the BSD Zero Clause License. See the `LICENSE` file in this repository for the full license text.
