#!/usr/bin/env python3
"""Add only our factory bank to this engine's cache; preserve every user bank."""
from pathlib import Path
import importlib.util
import json
import os
ROOT=Path(__file__).resolve().parents[1]
def main():
    spec=importlib.util.spec_from_file_location('weave_factory',ROOT/'scripts/factory-presets.py')
    factory=importlib.util.module_from_spec(spec);spec.loader.exec_module(factory)
    config=Path(os.environ.get('ZYNTHIAN_CONFIG_DIR','/zynthian/config'))
    path=config/'jalv/presets_Eka_Weave-4.json'
    original=path.read_bytes() if path.exists() else None
    banks=json.loads(original) if original else {}
    assert isinstance(banks,dict)
    label='Weave-4 Factory';bank_uri=factory.URI+'#factory'
    assert label not in banks or banks[label].get('bank_url')==bank_uri,'Factory bank label collision'
    bank={'bank_url':bank_uri,'presets':[{'label':title,'url':factory.URI+'#preset-'+slug} for slug,title,_ in factory.factory()]}
    if banks.get(label)==bank:
        print('Factory bank already registered')
        ensure_user_presets_bundle()
        return
    before={k:v for k,v in banks.items() if k!=label}
    banks[label]=bank
    assert {k:v for k,v in banks.items() if k!=label}==before
    backup=ROOT/'build/preset-cache-before-factory.json'
    if original is not None and not backup.exists():backup.write_bytes(original)
    path.parent.mkdir(parents=True,exist_ok=True)
    temp=path.with_name(path.name+'.weave.tmp')
    temp.write_text(json.dumps(banks,indent=0,ensure_ascii=False)+'\n')
    if path.exists():os.chmod(temp,path.stat().st_mode & 0o777)
    temp.replace(path)
    print({'factory_presets':len(bank['presets']),'existing_banks_preserved':len(before)})
    ensure_user_presets_bundle()

def ensure_user_presets_bundle():
    my_data=Path(os.environ.get('ZYNTHIAN_MY_DATA_DIR','/zynthian/zynthian-my-data'))
    bundle=my_data/'presets/lv2/Eka_Weave-4.presets.lv2'
    manifest=bundle/'manifest.ttl'
    if manifest.exists():
        return
    bundle.mkdir(parents=True,exist_ok=True)
    manifest.write_text(
        '@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n'
        '@prefix pset: <http://lv2plug.in/ns/ext/presets#> .\n'
        '@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .\n'
        '@prefix atom: <http://lv2plug.in/ns/ext/atom#> .\n'
        '@prefix rdf: <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .\n'
        '@prefix state: <http://lv2plug.in/ns/ext/state#> .\n'
        '@prefix xsd: <http://www.w3.org/2001/XMLSchema#> .\n'
    )
    print({'initialized_user_presets_bundle':str(manifest)})
if __name__=='__main__':main()
