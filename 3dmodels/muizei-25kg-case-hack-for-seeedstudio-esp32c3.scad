
$fn=60;

module xiao(){
translate([-74,-17.7,2.16]) import("/home/mauricio/Documents/PlatformIO/Projects/servo-motion-controller/3dmodels/Seeed Studio XIAO ESP32C3.stl");
}


//%translate([5,0,1]) xiao();

difference(){
    linear_extrude(17) offset(1.5) offset(-1.5) square([40,20],center=true);
  
    translate([5,0,3.5]) linear_extrude(16) offset(.2) projection(cut=true) translate([0,0,-3]) hull() xiao();
    
    
    translate([36,15]/2) cylinder(d=2.5,h=30,center=true);
    translate([-36,15]/2) cylinder(d=2.5,h=30,center=true);
    translate([36,-15]/2) cylinder(d=2.5,h=30,center=true);
    translate([-36,-15]/2) cylinder(d=2.5,h=30,center=true);
    
    translate([0,0,17-1.5]) linear_extrude(5) offset(1.5) offset(-1.5) square([40-1,20-1],center=true);
    
    translate([11,0,6]) rotate([0,90])linear_extrude(10) hull(){
        translate([0,5]) circle(d=7);
        translate([0,-5]) circle(d=7);
    }
    
   translate([-4,0,4]) rotate([0,-90]) hull(){
        cylinder(d=3,h=30);
        translate([20,0]) cylinder(d=3,h=30);
    }
    
    translate([-4.3,0,2]) cylinder(d=5,h=10);
    
translate([-9,0,5]) cylinder(d=18,h=10);
    
    
    translate([36,15]/2) cylinder(d=7,h=19,center=true);
    translate([-36,15]/2) cylinder(d=7,h=19,center=true);
    translate([36,-15]/2) cylinder(d=7,h=19,center=true);
    translate([-36,-15]/2) cylinder(d=7,h=19,center=true);
    
    
}