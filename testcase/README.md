# QQR phone test case

Run every command from this folder. `qqr` is `..\build\app\Release\qqr.exe`.

| File | Size | SHA-256 |
|---|---|---|
| `small.txt` | 5,006 bytes | `ACE11149338A23938F4BCE14278DC7B4789B75303B529606D4A05AAEC118F7A0` |
| `folder.tar.gz` | 1,602,997 bytes | `58C8D8C82C3068028F23076F40BCFBD4800AFFFA499601700824ED8FEC9529C5` |

`folder.tar.gz` packs `folder\`: two text files and 1.5 MB of random data, so it does not compress further.

## 1. Calibrate (about 90 s)

1. Mount the phone where it will stay, with the whole screen in view. Set it to 4K at 60 fps.
2. Run `qqr calibrate show`. It tries 7 cell sizes with 4 and 2 colours at 30 and 20 frames/s, 3 s each.
   Start recording, press Space, wait one full cycle (84 s) plus 5 s, press Esc.
3. Copy the video here and run `qqr calibrate check calib.mp4`.
4. Note the recommended `--cell`, `--colors` and `--fps`. Step 3 below uses them as `N`, `C` and `F`.

To narrow or widen the sweep, give lists, up to 32 combinations in all; for example
`qqr calibrate show --cells 6,5,4,3 --colors 4,2 --fps 30,20,15`. `check` reads the plan from the frames.

## 2. Single photo

1. Open `small.txt.png` at 100% zoom, full screen, on a 1920x1080 display.
2. Take a photo as JPEG (on iPhone: Settings > Camera > Formats > Most Compatible).
3. Run `qqr decode photo.jpg --out received`.
4. Pass: it prints `SHA-256 verified`, and `received\small.txt` has the hash above.

## 3. Video transfer

1. Run `qqr play folder.tar.gz --cell N --colors C --fps F`. It prints the length of one pass.
2. Start recording, press Space. When the `pass` counter at the bottom of the screen passes 1.00, wait about 10 s
   more and press Esc.
3. Run `qqr decode video.mp4 --colors C --out received`.
4. Pass: it prints `SHA-256 verified`, and `tar -xzf received\folder.tar.gz` gives back `folder\`.
5. If it lists missing blocks: run `play` again, record another pass, and decode both videos together:
   `qqr decode video.mp4 video2.mp4 --out received`.

## What to send back when a step fails

- The full console output of `decode` or `calibrate check`.
- One frame from the video: `ffmpeg -ss 5 -i video.mp4 -frames:v 1 frame.png`.
