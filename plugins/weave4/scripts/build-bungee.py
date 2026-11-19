#!/usr/bin/env python3
"""Build pinned private Bungee with a causal autoOut stream adapter."""
from pathlib import Path
import concurrent.futures
import hashlib
import io
import shutil
import subprocess
import tarfile
import urllib.request
ROOT=Path(__file__).resolve().parents[1]
BASE=ROOT/'build/bungee'
REV='61f52bf34be577d189d388772ddfa3696dee77d8'
PACKAGES=[('bungee', 'https://codeload.github.com/bungee-audio-stretch/bungee/tar.gz/61f52bf34be577d189d388772ddfa3696dee77d8', '33a00cb1b8a47dd6bc012ee98c419205243e052d1880aca5ff08efea19064566'), ('eigen', 'https://codeload.github.com/eigenteam/eigen-git-mirror/tar.gz/c29c800126982c561e8d0b9255dc65474cd98de3', '57bad130ebe3ec0eda84e0cf998b9c5083473e520e96a97616bc69ca08c68514'), ('pffft', 'https://bitbucket.org/jpommier/pffft/get/02fe7715a5bf8bfd914681c53429600f94e0f536.tar.gz', '9adeb18ac7bb52e9fb921c31c0c6a4e9ae150cc6fcb20a899d4b3a2275176ded')]

def main():
    BASE.mkdir(parents=True,exist_ok=True)
    for name,url,sha in PACKAGES:
        archive=BASE/(name+'.tar.gz')
        if not archive.exists():archive.write_bytes(urllib.request.urlopen(url,timeout=30).read())
        raw=archive.read_bytes()
        assert hashlib.sha256(raw).hexdigest()==sha
        dest=BASE/name
        if not dest.exists():
            temp=BASE/('extract-'+name);temp.mkdir(exist_ok=True)
            with tarfile.open(fileobj=io.BytesIO(raw)) as t:t.extractall(temp,filter='data')
            next(temp.iterdir()).rename(dest)
    source=BASE/'bungee'
    header=source/'bungee/Stream.h'
    text=header.read_text()
    if 'void resetLive()' not in text:
        # autoOut does not resample the input. The FFT half-window is sufficient
        # lookahead, unlike upstream's reserve for all +/-two-octave input modes.
        text=text.replace('inputBuffer.stretcher.maxInputFrameCount() / 2','inputBuffer.stretcher.maxInputFrameCount() / 8')
        marker='\t\tauto endPosition() const'
        assert marker in text
        text=text.replace(marker,'\t\tvoid resetLive() { begin=end=0; inputChunk={}; std::fill(buffer.begin(),buffer.end(),0.f); }\n'+marker,1)
        marker='\tint process(\n'
        assert marker in text
        reset="""\tvoid resetLive() {
        Request invalid{}; invalid.position=std::numeric_limits<double>::quiet_NaN();
        invalid.speed=invalid.pitch=1.; invalid.reset=true;
        OutputChunk discarded{};
        if (!std::isnan(request.position)) { inputBuffer.analyseGrain(); inputBuffer.stretcher.synthesiseGrain(discarded); }
        for(int i=0;i<8;i++) { inputBuffer.stretcher.specifyGrain(invalid); inputBuffer.stretcher.analyseGrain(nullptr,0,0,0); inputBuffer.stretcher.synthesiseGrain(discarded); }
        inputBuffer.resetLive(); request={}; request.position=std::numeric_limits<double>::quiet_NaN();
        outputChunk={}; outputChunkConsumed=0; framesNeeded=0.;
    }
"""
        text=text.replace(marker,reset+marker,1);header.write_text(text)
    objects=BASE/'objects';objects.mkdir(exist_ok=True)
    common=['g++','-O3','-DNDEBUG','-fPIC','-std=c++20','-fwrapv','-DBUNGEE_SELF_TEST=0','-Deigen_assert=BUNGEE_ASSERT1','-DEIGEN_DONT_PARALLELIZE=1','-DBUNGEE_VERSION="'+REV+'"','-I'+str(source),'-I'+str(BASE/'eigen'),'-I'+str(BASE/'pffft')]
    queue=[]
    for path in sorted((source/'src').glob('*.cpp')):
        out=objects/(path.name+'.o');queue.append((out,common+['-c',str(path),'-o',str(out)]))
    for name in ('pffft.c','fftpack.c'):
        out=objects/(name+'.o');queue.append((out,['cc','-O3','-fPIC','-ffast-math','-fno-finite-math-only','-c',str(BASE/'pffft'/name),'-o',str(out)]))
    def compile_one(item):
        out,cmd=item
        if not out.exists():
            p=subprocess.run(cmd,capture_output=True,text=True)
            if p.returncode:raise RuntimeError(p.stderr)
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:list(pool.map(compile_one,queue))
    subprocess.run(['ar','rcs',str(BASE/'libbungee.a')]+[str(out) for out,_ in queue],check=True)
    notices=BASE/'notices';notices.mkdir(exist_ok=True)
    shutil.copyfile(source/'LICENSE',notices/'LICENSE-Bungee-MPL2.txt')
    for path in (BASE/'eigen').glob('COPYING*'):
        shutil.copyfile(path,notices/('LICENSE-Eigen-'+path.name+'.txt'))
    text=(BASE/'pffft/pffft.c').read_text()
    (notices/'LICENSE-PFFFT-FFTPACK.txt').write_text(text[:text.index('*/')+2]+'\n')
    print(BASE/'libbungee.a')
if __name__=='__main__':main()
