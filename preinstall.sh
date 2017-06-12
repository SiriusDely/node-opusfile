OPUSFILE_DIR="./opusfile"

if [ -d $OPUSFILE_DIR ]; then
  rm -rf $OPUSFILE_DIR
fi

git clone https://github.com/SmartWalkieOrg/opusfile.git --depth=1
node-gyp rebuild
