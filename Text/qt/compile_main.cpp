// SPDX-License-Identifier: MIT
#include "scenecompiler.hpp"
#include <QGuiApplication>
#include <QFile>
#include <cstdio>
int main(int argc,char **argv)
{
    QGuiApplication app(argc,argv);
    const auto args=app.arguments();
    if(args.size()!=5){std::fprintf(stderr,"Usage: studio-text-compile settings.json output.stxt width height\n");return 2;}
    try {
        QFile source(args[1]);
        if(!source.open(QIODevice::ReadOnly)||source.size()>128*1024) throw std::runtime_error("Cannot read settings (maximum 128 KiB)");
        QJsonParseError parse;
        const auto json=QJsonDocument::fromJson(source.readAll(),&parse);
        if(parse.error!=QJsonParseError::NoError || !json.isObject()) throw std::runtime_error("Settings must be a JSON object");
        const auto scene=SunimoTextQt::compile(json.object(),QSize(args[3].toInt(),args[4].toInt()));
        QString error;
        if(!SunimoTextQt::save(args[2],scene,&error)) throw std::runtime_error(error.toStdString());
        return 0;
    } catch(const std::exception &e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
