#!/usr/bin/env python3
"""Generate complete, deterministic HQ factory presets; no user preset writes."""
from pathlib import Path
import re
import sys
ROOT=Path(__file__).resolve().parents[1]
URI="http://dmtry3.github.io/zynthian-eka/weave4"
def defaults():
    result={}
    for part in re.split(r"(?=    \[ a lv2:(?:Input|Output)Port)",(ROOT/"lv2/eka_weave4.ttl").read_text()):
        if not part.startswith("    [ a lv2:InputPort, lv2:ControlPort"):
            continue
        symbol=re.search(r'lv2:symbol "([^"]+)"',part)
        value=re.search(r'lv2:default (-?[0-9.]+)',part)
        if symbol and value:result[symbol.group(1)]=float(value.group(1))
    assert len(result)>250
    return result

def factory():
    base=defaults()
    base.update(dry=-3,master=-2,dry_timing=0,width=0,scene=0,solo=0,selected_voice=1,voices=1,
                enable1=1,enable2=0,enable3=0,enable4=0)
    def make(slug,title,values):
        data=dict(base);data.update(values)
        assert data.keys()==base.keys(),set(data)-set(base)
        return slug,title,data
    presets=[]
    presets.append(make('guitar-c-thirds','01 Guitar - C Major Thirds',dict(
        mode=3,scale_tonic=0,scale_mode=0,degree1=2,wet=-9,tracking_profile=1,
        hpf1=80,lpf1=9000,tone_on1=1)))
    presets.append(make('guitar-low-octave','02 Guitar - Low Octave',dict(
        mode=0,pitch=-12,wet=-12,tracking_profile=1,hpf1=50,lpf1=3500,tone_on1=1)))
    presets.append(make('guitar-wide-fifths','03 Guitar - Wide Fifths',dict(
        mode=0,pitch=7,pitch2=-5,wet=-15,level2=-18,voices=2,enable2=1,pan1=-.5,pan2=.5,
        tracking_profile=1,hpf1=100,hpf2=100,lpf1=7500,lpf2=7500,tone_on1=1,tone_on2=1)))
    presets.append(make('flute-d-thirds','04 Flute - D Minor Thirds',dict(
        mode=3,scale_tonic=2,scale_mode=1,degree1=2,wet=-12,dry=-2,tracking_profile=2,
        humanize1=6,human_speed1=.18,hpf1=180,lpf1=8000,tone_on1=1)))
    presets.append(make('flute-airy','05 Flute - Airy Octave',dict(
        mode=0,pitch=12,pitch2=7,wet=-18,level2=-21,voices=2,enable2=1,pan1=-.6,pan2=.6,
        tracking_profile=2,humanize1=8,humanize2=8,fine1=-4,fine2=4,
        hpf1=350,hpf2=350,lpf1=9500,lpf2=9500,tone_on1=1,tone_on2=1)))
    ensemble=dict(mode=0,voices=4,dry=-6,master=-3,pitch=0,pitch2=0,pitch3=0,pitch4=0,
                  wet=-18,level2=-18,level3=-18,level4=-18,enable2=1,enable3=1,enable4=1)
    for v,(pan,fine) in enumerate(zip((-.8,.8,-.3,.3),(-7,7,-3,3)),1):
        ensemble.update({f'pan{v}':pan,f'fine{v}':fine,f'humanize{v}':25,f'human_speed{v}':.15})
    presets.append(make('gentle-ensemble','06 Ensemble - Four Gentle Voices',ensemble))
    presets.append(make('dark-minor-echoes','07 Dark - Minor Echoes',dict(
        mode=1,chord=1,voicing=1,voices=2,enable2=1,wet=-15,level2=-18,master=-3,
        pan1=-.6,pan2=.6,delay_ms1=375,delay_ms2=550,feedback1=35,feedback2=28,
        hpf1=150,hpf2=150,lpf1=2000,lpf2=2000,tone_on1=1,tone_on2=1)))
    presets.append(make('rhythm-fifths','08 Rhythm - Synced Fifths',dict(
        mode=0,pitch=7,pitch2=12,voices=2,enable2=1,wet=-15,level2=-18,master=-3,
        pan1=-.65,pan2=.65,sync1=1,sync2=1,division1=9,division2=11,feedback1=35,feedback2=25,
        hpf1=120,hpf2=120,lpf1=5000,lpf2=5000,tone_on1=1,tone_on2=1)))
    choir=dict(mode=2,voices=4,wet=-15,level2=-15,level3=-15,level4=-15,master=-3,
               enable2=1,enable3=1,enable4=1)
    for v,(pan,fine) in enumerate(zip((-.7,.7,-.25,.25),(-3,3,-2,2)),1):
        choir.update({f'pan{v}':pan,f'fine{v}':fine,f'humanize{v}':8})
    presets.append(make('midi-choir','09 MIDI - Four Voice Choir',choir))
    return presets

def main():
    bundle=Path(sys.argv[1]) if len(sys.argv)>1 else ROOT/'build/eka-weave4.lv2'
    assert bundle.is_dir()
    header='@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n@prefix pset: <http://lv2plug.in/ns/ext/presets#> .\n'
    text=header+f'<{URI}#factory> a pset:Bank ; rdfs:label "Weave-4 Factory" ; lv2:appliesTo <{URI}> .\n'
    refs='\n@prefix pset: <http://lv2plug.in/ns/ext/presets#> .\n'
    refs+=f'<{URI}#factory> a pset:Bank ; rdfs:label "Weave-4 Factory" ; lv2:appliesTo <{URI}> ; rdfs:seeAlso <factory-presets.ttl> .\n'
    for slug,title,values in factory():
        subject=URI+'#preset-'+slug
        refs+=f'<{subject}> a pset:Preset ; lv2:appliesTo <{URI}> ; rdfs:seeAlso <factory-presets.ttl> .\n'
        text+=f'<{subject}> a pset:Preset ; lv2:appliesTo <{URI}> ; pset:bank <{URI}#factory> ; rdfs:label "{title}" ; lv2:port\n'
        text+=',\n'.join(' [ lv2:symbol "%s" ; pset:value %s ]'%(symbol,format(value,'.8g')) for symbol,value in values.items())+' .\n'
    (bundle/'factory-presets.ttl').write_text(text)
    manifest=bundle/'manifest.ttl'
    original=manifest.read_text().split('\n@prefix pset:',1)[0]
    manifest.write_text(original+refs)
if __name__=='__main__':main()
