# mpeg1_player

Play mpeg1 video (mpeg1video only)

## Compile
```
cmake . && make
```

## Run
```
./mpeg1_player video.mpg
```

## FFMPEG

Encode video into suitable format uisng ffmpeg
```
ffmpeg -i input.mp4 -c:v mpeg1video -c:a mp2 -format mpeg output.mpg
```
