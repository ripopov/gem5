# rainbow-image

`rainbow-image` is a small HIP C++ image-generation example. It launches a
two-dimensional grid over a 320x240 image. Each GPU work-item computes an RGB
rainbow color from its pixel coordinates and writes exactly one pixel.

After the kernel completes, the host copies the pixels back, writes a 24-bit
BMP to `/tmp/rainbow-image.bmp`, and exports it to the gem5 run directory with
`/sbin/m5 writefile`. This keeps the GPU kernel focused on image generation
while showing the complete path for getting a generated image out of a guest.

## Build

From `gpu-lab/workspace`:

```sh
./scripts/build-samples.sh --backend simdojo
```

The binary is written to `work/apps/simdojo/rainbow-image`. Replace `simdojo`
with `gem5-kmd` or `full-system` when preparing another backend.

## Run

From `gpu-lab/workspace`:

```sh
./scripts/run-sample.sh --backend simdojo rainbow-image
```

The run script checks the serial PASS line and validates that the exported
`rainbow-image.bmp` is a 320x240, 24-bit BMP with the expected pixel checksum.
The image is placed in the run output directory, for example:

```text
work/runs/rainbow-image-20260715-120000/rainbow-image.bmp
```

Expected application output is:

```text
rainbow_image: filled 320x240 checksum=0x941d233a32bf8839
rainbow_image: PASS image=rainbow-image.bmp
```
