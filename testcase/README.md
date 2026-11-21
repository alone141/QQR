# VFDT phone test case

Run every command from this folder. `vfdt` is `..\build\app\Release\vfdt.exe`.

| File | Size | SHA-256 |
|---|---|---|
| `small.txt` | 5,006 bytes | `ACE11149338A23938F4BCE14278DC7B4789B75303B529606D4A05AAEC118F7A0` |
| `folder.tar.gz` | 1,602,997 bytes | `58C8D8C82C3068028F23076F40BCFBD4800AFFFA499601700824ED8FEC9529C5` |

`folder.tar.gz` packs `folder\`: two text files and 1.5 MB of random data, so it does not compress further.

## 1. Calibrate (about 30 s)

1. Mount the phone where it will stay, with the whole screen in view. Set it to 60 fps.
2. Run `vfdt calibrate show`, start recording, press Space, wait one full cycle (21 s) plus 5 s, press Esc.
3. Copy the video here and run `vfdt calibrate check calib.mp4`.
4. Note the recommended `--cell` value. Steps 2 and 3 below use it as `N`.

## 2. Single photo

1. Open `small.txt.png` at 100% zoom, full screen, on a 1920x1080 display.
2. Take a photo as JPEG (on iPhone: Settings > Camera > Formats > Most Compatible).
3. Run `vfdt decode photo.jpg --out received`.
4. Pass: it prints `SHA-256 verified`, and `received\small.txt` has the hash above.

## 3. Video transfer

1. Run `vfdt play folder.tar.gz --cell N`. It prints the length of one pass.
2. Start recording, press Space, record one pass plus about 10 s, press Esc.
3. Run `vfdt decode video.mp4 --out received`.
4. Pass: it prints `SHA-256 verified`, and `tar -xzf received\folder.tar.gz` gives back `folder\`.
5. If it lists missing blocks: run `play` again, record another pass, and decode both videos together:
   `vfdt decode video.mp4 video2.mp4 --out received`.

## What to send back when a step fails

- The full console output of `decode` or `calibrate check`.
- One frame from the video: `ffmpeg -ss 5 -i video.mp4 -frames:v 1 frame.png`.
