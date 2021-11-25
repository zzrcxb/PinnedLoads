#!/bin/bash

# Add local user
# Either use the LOCAL_USER_ID if passed in at runtime or
# fallback

USER_ID=${LOCAL_USER_ID:-9001}

echo "Starting with UID : $USER_ID"
useradd --shell /bin/bash -u $USER_ID -d $GEM5_ROOT -o -c "" -m user
cp /etc/sudoers /etc/sudoers.bak
echo 'user  ALL=(root) NOPASSWD: ALL' >> /etc/sudoers

export HOME=$GEM5_ROOT

echo "Home is $HOME, GEM5 is $GEM5_ROOT"
echo "Execute $@"
exec /usr/sbin/gosu user "$@"
