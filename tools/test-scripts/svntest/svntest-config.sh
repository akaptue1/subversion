#!/bin/sh

#
# Root of the test tree
#
TEST_ROOT="/home/brane/svn"

#
# Repository paths
#
SVN_REPO="$TEST_ROOT/repo"
APR_REPO="$TEST_ROOT/apr"
APU_REPO="$TEST_ROOT/apr-util"
HTTPD_REPO="$TEST_ROOT/httpd-2.0"

#
# Log file name prefix
#
LOG_FILE_PREFIX="$TEST_ROOT/LOG_svntest"

#
# Configure script prefix and object directory names
#
CONFIG_PREFIX="config.solaris"
OBJ_STATIC="obj-st"
OBJ_SHARED="obj-sh"

#
# E-mail addresses for reporting
#
FROM="brane@xbc.nu"
TO="svn-breakage@subversion.tigris.org"
ERROR_TO="brane@hermes.si"
REPLY_TO="dev@subversion.tigris.org"

#
# Path to utilities
#
BIN="/usr/bin"
LOCALBIN="/usr/local/bin"

# Statically linked svn binary (used for repository updates)
SVN="$TEST_ROOT/inst/bin/svn"

# CVS binary (used for updating APR & friends)
CVS="$LOCALBIN/cvs"

# Path to config.guess (used for generating the mail subject line)
GUESS="/usr/local/share/libtool/config.guess"

# Path to sendmail
SENDMAIL="/usr/lib/sendmail"

# Other stuff
BASE64_E="$BIN/base64 -e - -"
CAT="$BIN/cat"
CP="$BIN/cp"
CP_F="$BIN/cp -f"
CUT="$BIN/cut"
GREP="$BIN/grep"
GZIP_C="$BIN/gzip -9c"
ID_UN="$BIN/id -un"
KILL="$BIN/kill"
MAKE="$LOCALBIN/make"
MKDIR="$BIN/mkdir"
NICE="$BIN/nice"
PS_U="$TOPBIN/ps -u"
RM="$BIN/rm"
RM_F="$BIN/rm -f"
RM_RF="$BIN/rm -rf"
SED="$BIN/sed"
TAIL="$BIN/tail"

#
# Helper functions
#

# Start a test
START() {
    TST="$1"
    echo ""
    echo "$2"
}

# Test failed
FAIL() {
    echo "FAIL: $TST" >> $LOG_FILE
    test -n "$1" && eval "$1"   # Run cleanup code
    exit 1
}

# Test passed
PASS() {
    echo "PASS: $TST" >> $LOG_FILE
}

# Copy a partial log to the main log file
FAIL_LOG() {
    echo >> $LOG_FILE
    echo "Last 100 lines of the log file follow:" >> $LOG_FILE
    $TAIL -100 "$1" >> $LOG_FILE 2>&1
    if [ "x$REV" = "x" ]
    then
        SAVED_LOG="$1.failed"
    else
        SAVED_LOG="$1.$REV.failed"
    fi
    $CP "$1" "$SAVED_LOG" >> $LOG_FILE 2>&1
    echo "Complete log saved in $SAVED_LOG" >> $LOG_FILE
}
