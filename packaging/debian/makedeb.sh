ROOT_PATH=$1
PACKAGE_PATH=$ROOT_PATH/packaging/debian/deb
CONTROL_PATH=$ROOT_PATH/packaging/debian/deb/DEBIAN

mkdir -p $CONTROL_PATH
make -C $ROOT_PATH install prefix=$PACKAGE_PATH

cp $ROOT_PATH/packaging/debian/control $CONTROL_PATH
# replace version info
ATOP_VERSION=`cat atop/version.h | tr "\"" " " | awk '{print $3}'`
sed -i "s/ATOP_VERSION/$ATOP_VERSION/" $CONTROL_PATH/control

# replace package size info
ATOPHTTPD_SIZE=`du -k -d 0 $PACKAGE_PATH | awk '{print $1}'`
sed -i "s/ATOPHTTPD_SIZE/$ATOPHTTPD_SIZE/" $CONTROL_PATH/control

dpkg-deb -b $PACKAGE_PATH $ROOT_PATH/atophttpd_$ATOP_VERSION.deb

# cleanup
rm -rf $PACKAGE_PATH
