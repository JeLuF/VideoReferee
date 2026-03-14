mkdir deploy
copy release\VideoReferee.exe deploy
windeployqt --release deploy\VideoReferee.exe

copy "release\libgcc_s_seh-1.dll" deploy\
copy "release\libstdc++-6.dll" deploy\
copy "release\opencv_videoio_ffmpeg4120_64.dll" deploy\
copy "release\libopencv_world4120.dll" deploy\
