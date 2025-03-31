# How to build

## Build C files
gcc -D_GNU_SOURCE etcdlock_manager.c libvirt_sanlock_helper.c monotime.c -o etcdlock_manager -lcurl

## Build Go files
go build etcd_client.go

# How to run
./etcdlock_manager
./etcd_client