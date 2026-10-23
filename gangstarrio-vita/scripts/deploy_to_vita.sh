#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
volume="/Volumes/Untitled"

echo "Waiting for PS Vita ($volume)..."
while [ ! -d "$volume" ]; do
    sleep 1
done

echo "PS Vita detected at $volume."

# Copy VPK to ux0:data/
if [ -f "$project_dir/build/gangstarrio-vita.vpk" ]; then
    echo "Copying VPKs to $volume/data/..."
    mkdir -p "$volume/data"
    cp "$project_dir/build/gangstarrio-vita.vpk" "$volume/data/gangstarrio-vita.vpk"
    cp "$project_dir/build/gangstarrio-vita.vpk" "$volume/data/RIO_011.vpk"
fi

# Update installed app in ux0:app/GSRV00001/ directly
if [ -d "$volume/app/GSRV00001" ]; then
    echo "Updating installed application in $volume/app/GSRV00001/..."
    cp "$project_dir/build/eboot.bin" "$volume/app/GSRV00001/eboot.bin"
    mkdir -p "$volume/app/GSRV00001/sce_sys/livearea/contents"
    cp "$project_dir/assets/livearea/startup.png" "$volume/app/GSRV00001/sce_sys/livearea/contents/startup.png"
    cp "$project_dir/assets/livearea/template.xml" "$volume/app/GSRV00001/sce_sys/livearea/contents/template.xml"
    cp "$project_dir/assets/livearea/bg0.png" "$volume/app/GSRV00001/sce_sys/livearea/contents/bg0.png"
fi

sync
echo "Deployment complete! Safely unmount or disconnect USB in VitaShell."
