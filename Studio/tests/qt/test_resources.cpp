// SPDX-License-Identifier: GPL-3.0-only
#include "../../overlay/core/studioresources.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <cassert>
#include <iostream>
int main(int argc,char**argv)
{
    QCoreApplication app(argc,argv);
    QTemporaryDir temp;assert(temp.isValid());
    const auto prefix=temp.path()+QString::fromUtf8("/префикс с пробелами");
    QDir().mkpath(prefix+"/libexec");QDir().mkpath(prefix+"/share/studio-subtitles");
    const auto worker=prefix+"/libexec/studio-missing-test-worker";
    {QFile file(worker);assert(file.open(QIODevice::WriteOnly));file.write("#!/bin/sh\nexit 0\n");}
    assert(QFile::setPermissions(worker,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
    const auto data=prefix+"/share/studio-subtitles/unit-test-model.bin";
    {QFile file(data);assert(file.open(QIODevice::WriteOnly));file.write("fixture");}
    qputenv("STUDIO_PREFIX",prefix.toUtf8());
    assert(StudioResources::executable("studio-missing-test-worker")==worker);
    assert(StudioResources::dataFile("studio-subtitles/unit-test-model.bin")==data);
    assert(StudioResources::dataFile("../escape").isEmpty());
    assert(StudioResources::executable("../sh").isEmpty());
    assert(StudioResources::dataFile("/etc/passwd").isEmpty());
    assert(StudioResources::executable("missing-studio-executable-79517").isEmpty());
    std::cout<<"Linux prefix, libexec, data lookup, UTF-8 paths and traversal rejection PASS\n";
}
