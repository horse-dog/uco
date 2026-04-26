#!/bin/bash

cd ..
zip -r uco.zip uco -x "uco/build/*" "uco/res/*"
echo "Done: uco.zip"
