mkdir deploy
copy release\VideoReferee.exe deploy
windeployqt --release release\VideoReferee.exe

copy release\libgcc_s_seh-1.dll release\libstdc++-6.dll release\opencv_videoio_ffmpeg4120_64.dll deploy\
