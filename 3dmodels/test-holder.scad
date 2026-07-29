$fn=30;

servos = 4;

linear_extrude(5)
difference(){
    translate([-20,-30]) offset(5) offset(-5) square([60*servos-20,60]);

for(i=[0:servos-1]) translate([i*60,0]) {
offset(1) offset(-1) offset(0.2) square([20,40],center=true);
    
    translate([10,48]/2) circle(d=4.5);
    translate([10,-48]/2) circle(d=4.5);
    translate([-10,48]/2) circle(d=4.5);
    translate([-10,-48]/2) circle(d=4.5);


    translate([30,48]/2) circle(d=4.5);
    translate([30,-48]/2) circle(d=4.5);
    translate([-30,48]/2) circle(d=4.5);
    translate([-30,-48]/2) circle(d=4.5);
    
    translate([30,0]) circle(d=4.5);
   
}
 
}