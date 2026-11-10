use std::{collections::HashMap, env, fs, path::Path, process};

#[derive(Clone, Copy, Debug, Default)]
struct V { x: f64, y: f64, z: f64 }
impl V {
    fn sub(self, b: V) -> V { V { x:self.x-b.x, y:self.y-b.y, z:self.z-b.z } }
    fn cross(self, b: V) -> V { V { x:self.y*b.z-self.z*b.y, y:self.z*b.x-self.x*b.z, z:self.x*b.y-self.y*b.x } }
    fn len(self) -> f64 { (self.x*self.x+self.y*self.y+self.z*self.z).sqrt() }
}
#[derive(Clone,Default)] struct Mesh { vertices: Vec<V>, faces: Vec<[usize;3]> }

fn parse_obj(s: &str) -> Result<Mesh,String> {
    let mut m=Mesh::default();
    for line in s.lines() {
        let mut p=line.split_whitespace();
        match p.next() {
            Some("v") => { let n:Vec<f64>=p.take(3).map(|x|x.parse::<f64>().unwrap_or(f64::NAN)).collect(); if n.len()==3 && n.iter().all(|x|x.is_finite()) { m.vertices.push(V{x:n[0],y:n[1],z:n[2]}); } }
            Some("f") => { let ids:Vec<usize>=p.filter_map(|x|x.split('/').next()?.parse::<isize>().ok()).filter_map(|i| if i>0 {Some(i as usize-1)} else if i<0 {m.vertices.len().checked_sub((-i) as usize)} else {None}).collect(); if ids.len()>=3 { for i in 1..ids.len()-1 { m.faces.push([ids[0],ids[i],ids[i+1]]); } } }
            _=>()
        }
    }
    if m.vertices.is_empty() || m.faces.is_empty() { return Err("No usable vertices or faces found in this OBJ.".into()); } Ok(m)
}
fn weld_exact(m:Mesh)->Mesh{
    let mut out=Mesh::default();let mut map:HashMap<(u64,u64,u64),usize>=HashMap::new();let mut remap=vec![0;m.vertices.len()];
    for (i,v) in m.vertices.iter().enumerate(){let key=(v.x.to_bits(),v.y.to_bits(),v.z.to_bits());let id=*map.entry(key).or_insert_with(||{out.vertices.push(*v);out.vertices.len()-1});remap[i]=id;}
    out.faces=m.faces.into_iter().filter_map(|f|if f.iter().all(|&i|i<remap.len()){Some([remap[f[0]],remap[f[1]],remap[f[2]]])}else{None}).collect();out
}
fn parse_stl(bytes:&[u8])->Result<Mesh,String>{
    if bytes.len()>=84 {
        let count=u32::from_le_bytes(bytes[80..84].try_into().unwrap()) as usize;
        if count>0 && 84usize.saturating_add(count.saturating_mul(50))<=bytes.len() {
            let mut m=Mesh::default();
            for t in 0..count { let base=84+t*50+12; let mut f=[0;3]; for j in 0..3 { let b=base+j*12; f[j]=m.vertices.len(); m.vertices.push(V{x:f32::from_le_bytes(bytes[b..b+4].try_into().unwrap()) as f64,y:f32::from_le_bytes(bytes[b+4..b+8].try_into().unwrap()) as f64,z:f32::from_le_bytes(bytes[b+8..b+12].try_into().unwrap()) as f64}); } m.faces.push(f); }
            return Ok(weld_exact(m))
        }
    }
    let text=String::from_utf8_lossy(bytes); let mut m=Mesh::default(); let mut tri=Vec::new();
    for line in text.lines(){ let p:Vec<_>=line.split_whitespace().collect(); if p.first()==Some(&"vertex")&&p.len()>=4 { if let (Ok(x),Ok(y),Ok(z))=(p[1].parse(),p[2].parse(),p[3].parse()){tri.push(V{x,y,z}); if tri.len()==3 {let i=m.vertices.len();m.vertices.extend_from_slice(&tri);m.faces.push([i,i+1,i+2]);tri.clear();}} } }
    if m.faces.is_empty(){Err("STL contains no readable triangles.".into())}else{Ok(weld_exact(m))}
}
fn optimize(mut m:Mesh,density:f64,curve_detail:f64)->Mesh{
    if m.faces.is_empty(){return m}
    let original=m.clone();
    let (mut lo,mut hi)=(V{x:f64::INFINITY,y:f64::INFINITY,z:f64::INFINITY},V{x:f64::NEG_INFINITY,y:f64::NEG_INFINITY,z:f64::NEG_INFINITY});
    for v in &m.vertices {lo.x=lo.x.min(v.x);lo.y=lo.y.min(v.y);lo.z=lo.z.min(v.z);hi.x=hi.x.max(v.x);hi.y=hi.y.max(v.y);hi.z=hi.z.max(v.z);}
    let diag=V{x:hi.x-lo.x,y:hi.y-lo.y,z:hi.z-lo.z}.len().max(1e-9);
    // A density of 100 keeps the source tessellation; lower values increase the weld radius.
    let pitch=diag*(1.0-density.clamp(5.0,100.0)/100.0).powf(1.35)*0.045/curve_detail.clamp(0.5,2.0);
    if pitch>1e-12 {
        // Keep source samples at visibly bent corners; flat areas can be clustered more freely.
        let mut incident=vec![Vec::<V>::new();m.vertices.len()];
        for f in &m.faces {
            if f.iter().any(|&i|i>=m.vertices.len()){continue}
            let n=m.vertices[f[1]].sub(m.vertices[f[0]]).cross(m.vertices[f[2]].sub(m.vertices[f[0]]));
            let len=n.len().max(1e-12);let unit=V{x:n.x/len,y:n.y/len,z:n.z/len};
            for &i in f {incident[i].push(unit);}
        }
        let curved:Vec<bool>=incident.iter().map(|ns|{
            if ns.len()<2{return false}let mut mean=V::default();
            for n in ns {mean.x+=n.x;mean.y+=n.y;mean.z+=n.z;}
            let len=mean.len().max(1e-12);mean=V{x:mean.x/len,y:mean.y/len,z:mean.z/len};
            ns.iter().any(|n|1.0-(n.x*mean.x+n.y*mean.y+n.z*mean.z)>0.025)
        }).collect();
        let mut sums:Vec<(V,usize)>=Vec::new(); let mut cells:HashMap<(i64,i64,i64),usize>=HashMap::new(); let mut remap=vec![0;m.vertices.len()];
        for (i,v) in m.vertices.iter().enumerate(){
            if curved[i] {let id=sums.len();sums.push((*v,1));remap[i]=id;continue}
            let key=(((v.x-lo.x)/pitch).floor() as i64,((v.y-lo.y)/pitch).floor() as i64,((v.z-lo.z)/pitch).floor() as i64);
            let id=*cells.entry(key).or_insert_with(||{sums.push((V::default(),0));sums.len()-1});
            sums[id].0.x+=v.x;sums[id].0.y+=v.y;sums[id].0.z+=v.z;sums[id].1+=1;remap[i]=id;
        }
        m.vertices=sums.into_iter().map(|(v,n)| V{x:v.x/n as f64,y:v.y/n as f64,z:v.z/n as f64}).collect();
        for f in &mut m.faces {for i in 0..3{f[i]=remap[f[i]];}}
    }
    // Remove repeated-index, zero-area, and duplicate triangles after welding.
    let mut unique=HashMap::new(); let mut faces=Vec::new();
    for mut f in m.faces { if f[0]>=m.vertices.len()||f[1]>=m.vertices.len()||f[2]>=m.vertices.len(){continue} if f[0]==f[1]||f[1]==f[2]||f[0]==f[2]{continue}
        let area=m.vertices[f[1]].sub(m.vertices[f[0]]).cross(m.vertices[f[2]].sub(m.vertices[f[0]])).len(); if area<diag*diag*1e-12{continue}
        let mut k=f;k.sort();if unique.insert(k,()).is_none(){faces.push(f)}
    }
    if faces.is_empty(){return original}
    m.faces=faces;m
}
fn output_obj(m:&Mesh)->String{let mut s=String::from("# FacetForge optimized mesh\n");for v in &m.vertices{s.push_str(&format!("v {:.7} {:.7} {:.7}\n",v.x,v.y,v.z));}for f in &m.faces{s.push_str(&format!("f {} {} {}\n",f[0]+1,f[1]+1,f[2]+1));}s}
fn run()->Result<(),String>{
    let args:Vec<String>=env::args().collect();
    if args.len()<3 || args.iter().any(|x|x=="--help"||x=="-h"){println!("FacetForge — clean and density-control OBJ/STL meshes\n\nUsage: facetforge <input.obj|input.stl> <output.obj> [--density 5..100] [--curve-detail 0.5..2]\n\nExports triangulated OBJ. Density 100 preserves source sampling; lower values merge nearby vertices and remove degenerate faces.");return Ok(())}
    let input=Path::new(&args[1]);let output=Path::new(&args[2]);let mut density=72.0;let mut curve=1.0;let mut i=3;
    while i<args.len(){match args[i].as_str(){"--density"=>{i+=1;density=args.get(i).ok_or("Missing density value")?.parse().map_err(|_|"Invalid density value")?},"--curve-detail"=>{i+=1;curve=args.get(i).ok_or("Missing curve-detail value")?.parse().map_err(|_|"Invalid curve-detail value")?},x=>return Err(format!("Unknown option: {x}"))}i+=1;}
    let bytes=fs::read(input).map_err(|e|format!("Cannot read input: {e}"))?;let ext=input.extension().and_then(|x|x.to_str()).unwrap_or("").to_ascii_lowercase();
    let mesh=if ext=="stl"{parse_stl(&bytes)?}else{parse_obj(&String::from_utf8_lossy(&bytes))?};let original=mesh.faces.len();let out=optimize(mesh,density,curve);fs::write(output,output_obj(&out)).map_err(|e|format!("Cannot write output: {e}"))?;
    eprintln!("FacetForge: {original} → {} triangles; wrote {}",out.faces.len(),output.display());Ok(())
}
fn main(){if let Err(e)=run(){eprintln!("facetforge: {e}");process::exit(2)}}

#[cfg(test)] mod tests { use super::*;
#[test] fn obj_fan_triangulates_and_cleans(){let m=parse_obj("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3 4\n").unwrap();assert_eq!(m.faces.len(),2);let o=optimize(m,100.0,1.0);assert_eq!(o.faces.len(),2);}
#[test] fn binary_stl_is_read(){let mut b=vec![0u8;84+50];b[80..84].copy_from_slice(&1u32.to_le_bytes());let pts=[[0f32,0.,0.],[1.,0.,0.],[0.,1.,0.]];for (i,p) in pts.iter().enumerate(){let at=96+i*12;for j in 0..3{b[at+j*4..at+j*4+4].copy_from_slice(&p[j].to_le_bytes());}}assert_eq!(parse_stl(&b).unwrap().faces.len(),1);}
}
