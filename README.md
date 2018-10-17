# findutils-x
=============

Customized findutils (from GNU findutils)

# Difference from Original?

* supports use of stdin option '-'.
* fast compared to original when using multiple paths

# Usage

Add use stdin option(-) from find

For example, directory structure

```
.
|-- aaa.txt
|-- bbb.cpp
|-- ddd.txt
|-- path.txt
`-- sub
    `-- ccc.txt
```

pathlist.txt

```
sub
aaa.txt
```

and run findx

```shell
$ cat pathlist.txt | findx -
sub
sub/ccc.txt
aaa.txt

$ cat pathlist.txt | findx - ddd.txt
sub
sub/ccc.txt
aaa.txt
ddd.txt

$ cat pathlist.txt | findx ddd.txt -
ddd.txt
sub
sub/ccc.txt
aaa.txt
```

# About repository

This repository is forked from http://ftp.gnu.org/pub/gnu/findutils/ - [findutils-4.6.0.tar.gz](http://ftp.gnu.org/pub/gnu/findutils/findutils-4.6.0.tar.gz)


# LICENSE

GPLv3: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>
see COPYING file
