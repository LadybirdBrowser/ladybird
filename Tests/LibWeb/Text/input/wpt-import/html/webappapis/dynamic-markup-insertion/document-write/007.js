t.step(function() {
         order.push(2);
         document.write("<script>t.step(function() {order.push(3)})</script>");
       });