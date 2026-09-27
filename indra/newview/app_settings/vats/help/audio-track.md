# Audio track

An animation can carry one audio track, so you can time a dance or a gesture to the music while you work. The waveform shows along the timeline and plays with the animation; beat markers and a beat grid help put keys on the beat. The audio stays in VATs: it is never put in the exported `.anim`.

> Related articles: [[Keys and timeline]], [[Time editing]], [[Projects and files]], [[Export to Second Life]]

## Usage

### Loading and removing audio

**File → Load Audio...**, or right-click the timeline and choose **Load Audio...**, then pick a WAV, MP3, FLAC or Ogg Vorbis file. The status bar shows its name and length, for example "Loaded song.ogg (93.4 s)". Loading another file replaces the first.

Right-click the timeline and choose **Remove Audio** to take it out; it is greyed out with "Load an audio file first" when there is none.

### Playing and scrubbing

The audio plays with the animation from wherever the playhead is. While stopped, scrubbing plays short snippets, so you can find a beat by ear.

### Sliding the audio

- **Ctrl+drag** the timeline to slide the audio earlier or later. A tooltip shows where it starts, for example "Audio starts at 1.25 s".
- Or set **Start (s)** in the timeline's right-click menu (−600 to 600 s). A positive value starts the audio later than frame 0; a negative one starts partway into the song.

Marked beats slide with the audio.

### Audio settings

The timeline's right-click menu has an **Audio** section:

| Setting | What it does |
|---|---|
| **Volume** | 0–2, 1 by default; also scales the waveform |
| **Start (s)** | Where the audio starts on the timeline |
| **BPM** | A beat grid every 60/BPM seconds; 0 turns it off |
| **Beat Grid Starts Here** | Puts a beat of the grid on the current frame (needs a BPM) |
| **Mark a Beat Here** | Adds a beat marker at the current frame (**B**) |
| **Clear Marked Beats** | Removes every beat marker |
| **Snap to Beats** | Makes scrubbing and range picks land on the nearest beat |

Each change is one undo step.

### Beats

There are two ways to get beats onto the timeline; they can be used together:

- **A beat grid**: type the song's **BPM**, scrub to a frame that is on a beat, and choose **Beat Grid Starts Here**. Grid lines appear every beat.
- **Tapped beats**: play the animation and press **B** on each beat. **Mark a Beat Here** marks the current frame, so it works while stopped too.

With **Snap to Beats** on, the playhead and **Shift+drag** ranges snap to a beat within 3 frames of the mouse.

### Saving

The audio is saved in the project as a path to the file, relative to the project where possible (like props), together with its start, volume, BPM and beats. The sound itself is not copied into the project; keep the file with it. See [[Projects and files]].

## Tips and tricks

- Set **Snap to Beats**, then key the big poses by scrubbing from beat to beat.
- Set the animation's frame rate so a beat lands on whole frames: at 120 BPM a beat is 0.5 s, which is 15 frames at 30 fps.
- To pair the sound with the animation in Second Life, upload the sound too and start both from the same script.
- [[Time editing]] moves keys but not the audio; slide the audio separately if you insert frames before it.

## Troubleshooting

### "Audio not found"

The project refers to an audio file that has moved or been deleted. Put the file back where it was, or load it again with **File → Load Audio...**.

### "Could not load the audio"

The file is not a WAV, MP3, FLAC or Ogg Vorbis file VATs can decode. Convert it to one of these (WAV is the safest) and load it again.

### No sound while playing

Check the **Volume** in the timeline's right-click menu, and that the audio starts where you expect (**Start (s)**): before the audio's start, and after it ends, there is nothing to play.

### The uploaded animation has no sound

That is how Second Life works: animations carry no sound. Upload the sound separately and play it from a script.

## App and viewer

> **Note:** The audio track is not yet available in the VATs Animator in the viewer.

## See also

- [[Keys and timeline]]
- [[Time editing]]
- [[Export to Second Life]]

Category: Animating
