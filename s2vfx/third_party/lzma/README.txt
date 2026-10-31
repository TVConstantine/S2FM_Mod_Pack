LZMA SDK 24.09, the C sources the compiler needs (decoder, encoder, match finder), unchanged.
From https://github.com/ip7z/7zip (tag 24.09, folder C/). Public domain (Igor Pavlov).
Built single threaded (Z7_ST), so the multithreaded match finder is not needed.
